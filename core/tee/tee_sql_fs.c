/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file implements the tee_file_operations structure for a secure
 * filesystem based on an SQLite database in normal world.
 * The atomicity of each operation is ensured by using SQL transactions.
 * The main purpose of the code below is to perform block encryption and
 * authentication of the file data, and properly handle seeking through the
 * file. One file (in the sense of struct tee_file_operations) maps to one
 * file in the SQL filesystem, and has the following structure:
 *
 * [       File meta-data       ][      Block #0        ][Block #1]...
 * [meta_header|sql_fs_file_meta][block_header|user data][        ]...
 *
 * meta_header and block_header are defined in tee_fs_key_manager.h.
 */

#include <assert.h>
#include <kernel/tee_common_unpg.h>
#include <kernel/thread.h>
#include <kernel/handle.h>
#include <kernel/mutex.h>
#include <mm/core_memprot.h>
#include <optee_msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_ext.h>
#include <sys/queue.h>
#include <tee/tee_cryp_provider.h>
#include <tee/tee_fs.h>
#include <tee/tee_fs_defs.h>
#include <tee/tee_fs_key_manager.h>
#include <trace.h>
#include <utee_defines.h>
#include <util.h>

/* TEE FS operation */
#define TEE_FS_OPEN       1
#define TEE_FS_CLOSE      2
#define TEE_FS_READ       3
#define TEE_FS_WRITE      4
#define TEE_FS_SEEK       5
#define TEE_FS_UNLINK     6
#define TEE_FS_RENAME     7
#define TEE_FS_TRUNC      8
#define TEE_FS_MKDIR      9
#define TEE_FS_OPENDIR   10
#define TEE_FS_CLOSEDIR  11
#define TEE_FS_READDIR   12
#define TEE_FS_RMDIR     13
#define TEE_FS_ACCESS    14
#define TEE_FS_LINK      15
#define TEE_FS_BEGIN     16 /* SQL FS: begin transaction */
#define TEE_FS_END       17 /* SQL FS: end transaction */

/* sql_fs_send_cmd 'mode' */
#define TEE_FS_MODE_NONE 0
#define TEE_FS_MODE_IN   1
#define TEE_FS_MODE_OUT  2

/* Block size for encryption */
#define BLOCK_SHIFT 12
#define BLOCK_SIZE (1 << BLOCK_SHIFT)

struct sql_fs_file_meta {
	size_t length;
};

/* File descriptor */
struct sql_fs_fd {
	struct sql_fs_file_meta meta;
	uint8_t encrypted_fek[TEE_FS_KM_FEK_SIZE];
	tee_fs_off_t pos;
	int fd; /* returned by normal world */
	int flags; /* open flags */
};

struct tee_fs_dir {
	int nw_dir;
	struct tee_fs_dirent d;
};

/* Container for file descriptors (struct sql_fs_fd) */
static struct handle_db fs_db = HANDLE_DB_INITIALIZER;

/*
 * Interface with tee-supplicant
 */

struct tee_fs_rpc {
	int op;
	int flags;
	int arg;
	int fd;
	uint32_t len;
	int res;
};

static int sql_fs_send_cmd(struct tee_fs_rpc *bf_cmd, void *data, uint32_t len,
			   uint32_t mode)
{
	TEE_Result ret;
	struct optee_msg_param params;
	paddr_t phpayload = 0;
	uint64_t cpayload = 0;
	struct tee_fs_rpc *bf;
	int res = -1;

	thread_rpc_alloc_payload(sizeof(struct tee_fs_rpc) + len,
				 &phpayload, &cpayload);
	if (!phpayload)
		return -1;

	if (!ALIGNMENT_IS_OK(phpayload, struct tee_fs_rpc))
		goto exit;

	bf = phys_to_virt(phpayload, MEM_AREA_NSEC_SHM);
	if (!bf)
		goto exit;

	memset(&params, 0, sizeof(params));
	params.attr = OPTEE_MSG_ATTR_TYPE_TMEM_INOUT;
	params.u.tmem.buf_ptr = phpayload;
	params.u.tmem.size = sizeof(struct tee_fs_rpc) + len;
	params.u.tmem.shm_ref = cpayload;

	/* fill in parameters */
	*bf = *bf_cmd;

	if (mode & TEE_FS_MODE_IN)
		memcpy((void *)(bf + 1), data, len);

	ret = thread_rpc_cmd(OPTEE_MSG_RPC_CMD_SQL_FS,
			     1, &params);
	/* update result */
	*bf_cmd = *bf;
	if (ret != TEE_SUCCESS)
		goto exit;

	if (mode & TEE_FS_MODE_OUT) {
		uint32_t olen = MIN(len, bf->len);

		memcpy(data, (void *)(bf + 1), olen);
	}

	res = 0;

exit:
	thread_rpc_free_payload(cpayload);
	return res;
}

static int sql_fs_access_rpc(const char *name, int mode)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;
	size_t len;

	DMSG("(%s, %d)...", name, mode);

	if (!name)
		goto exit;

	len = strlen(name) + 1;

	head.op = TEE_FS_ACCESS;
	head.flags = mode;

	res = sql_fs_send_cmd(&head, (void *)name, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_begin_transaction_rpc(void)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("...");

	/* fill in parameters */
	head.op = TEE_FS_BEGIN;
	head.fd = -1;

	res = sql_fs_send_cmd(&head, NULL, 0, TEE_FS_MODE_NONE);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_close_rpc(int fd)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("(%d)...", fd);

	head.op = TEE_FS_CLOSE;
	head.fd = fd;

	res = sql_fs_send_cmd(&head, NULL, 0, TEE_FS_MODE_NONE);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_end_transaction_rpc(bool rollback)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("(%d)...", rollback);

	head.op = TEE_FS_END;
	head.arg = rollback;
	head.fd = -1;

	res = sql_fs_send_cmd(&head, NULL, 0, TEE_FS_MODE_NONE);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_ftruncate_rpc(int fd, tee_fs_off_t length)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("(%d, %" PRId64 ")...", fd, length);

	head.op = TEE_FS_TRUNC;
	head.fd = fd;
	head.arg = length;

	res = sql_fs_send_cmd(&head, NULL, 0, TEE_FS_MODE_NONE);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static tee_fs_off_t sql_fs_lseek_rpc(int fd, tee_fs_off_t offset, int whence)
{
	struct tee_fs_rpc head = { 0 };
	tee_fs_off_t rc = -1;
	TEE_Result res;

	DMSG("(%d, %" PRId64 ", %d)...", fd, offset, whence);

	head.op = TEE_FS_SEEK;
	head.fd = fd;
	head.arg = offset;
	head.flags = whence;

	res = sql_fs_send_cmd(&head, NULL, 0, TEE_FS_MODE_NONE);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%" PRId64, rc);
	return rc;
}

static int sql_fs_mkdir_rpc(const char *path, tee_fs_mode_t mode)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	uint32_t len;
	int rc = -1;

	DMSG("(%s, %d)...", path, mode);

	if (!path)
		return -1;

	len = strlen(path) + 1;

	head.op = TEE_FS_MKDIR;
	head.flags = mode;

	res = sql_fs_send_cmd(&head, (void *)path, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_open_rpc(TEE_Result *errno, const char *file, int flags, ...)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;
	size_t len;

	DMSG("(%s, %d)...", file, flags);

	len = strlen(file) + 1;

	head.op = TEE_FS_OPEN;
	head.flags = flags;

	res = sql_fs_send_cmd(&head, (void *)file, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS) {
		*errno = TEE_ERROR_GENERIC;
		res = -1;
		goto exit;
	}
	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static struct tee_fs_dir *sql_fs_opendir_rpc(const char *name)
{
	struct tee_fs_rpc head = { 0 };
	struct tee_fs_dir *dir = NULL;
	size_t len;
	TEE_Result res = TEE_SUCCESS;

	DMSG("(%s)...", name);

	if (!name)
		goto exit;

	len = strlen(name) + 1;

	dir = malloc(sizeof(struct tee_fs_dir));
	if (!dir)
		goto exit;

	head.op = TEE_FS_OPENDIR;

	res = sql_fs_send_cmd(&head, (void *)name, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto free_and_exit;
	if (head.res < 0)
		goto free_and_exit;

	dir->nw_dir = head.res;
	dir->d.d_name = NULL;

	goto exit;

free_and_exit:
	free(dir);
	dir = NULL;
exit:
	DMSG("...%p", (void *)dir);
	return dir;
}

static int sql_fs_read_rpc(int fd, void *buf, size_t len)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("(%d, %p, %zu)...", fd, (void *)buf, len);

	if (!len) {
		res = 0;
		goto exit;
	}

	if (!buf)
		goto exit;

	head.op = TEE_FS_READ;
	head.fd = fd;
	head.len = (uint32_t) len;

	res = sql_fs_send_cmd(&head, (void *)buf, len, TEE_FS_MODE_OUT);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static struct tee_fs_dirent *sql_fs_readdir_rpc(struct tee_fs_dir *d)
{
	struct tee_fs_dirent *rc = NULL;
	char fname[TEE_FS_NAME_MAX + 1];
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;

	DMSG("(%p)...", (void *)d);

	if (!d)
		goto exit;

	head.op = TEE_FS_READDIR;
	head.arg = (int)d->nw_dir;
	head.len = sizeof(fname);

	res = sql_fs_send_cmd(&head, fname, sizeof(fname), TEE_FS_MODE_OUT);
	if (res != TEE_SUCCESS)
		goto exit;

	if (head.res < 0)
		goto exit;

	if (!head.len || head.len > sizeof(fname))
		goto exit;

	fname[head.len - 1] = '\0'; /* make sure it's zero terminated */
	free(d->d.d_name);
	d->d.d_name = strdup(fname);
	if (!d->d.d_name)
		goto exit;

	rc = &d->d;
exit:
	DMSG("...%p", (void *)rc);
	return rc;
}

static int sql_fs_rename_rpc(const char *old, const char *nw)
{
	size_t len_old = strlen(old) + 1;
	size_t len_new = strlen(nw) + 1;
	size_t len = len_old + len_new;
	struct tee_fs_rpc head = { 0 };
	char *tmp = NULL;
	TEE_Result res;
	int rc = -1;

	DMSG("(%s, %s)...", old, nw);

	tmp = malloc(len);
	if (!tmp) {
		EMSG("REE FS: failed to allocate memory for rename");
		goto exit;
	}
	memcpy(tmp, old, len_old);
	memcpy(tmp + len_old, nw, len_new);

	head.op = TEE_FS_RENAME;

	res = sql_fs_send_cmd(&head, tmp, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	free(tmp);
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_write_rpc(int fd, const void *buf, size_t len)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("(%d, %p, %zu)...", fd, (void *)buf, len);

	if (!len) {
		res = 0;
		goto exit;
	}

	if (!buf)
		goto exit;

	head.op = TEE_FS_WRITE;
	head.fd = fd;
	head.len = (uint32_t) len;

	res = sql_fs_send_cmd(&head, (void *)buf, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_closedir_rpc(struct tee_fs_dir *d)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;

	DMSG("(%p)...", (void *)d);

	if (!d) {
		rc = 0;
		goto exit;
	}

	head.op = TEE_FS_CLOSEDIR;
	head.arg = (int)d->nw_dir;

	res = sql_fs_send_cmd(&head, NULL, 0, TEE_FS_MODE_NONE);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	if (d)
		free(d->d.d_name);
	free(d);

	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_rmdir_rpc(const char *name)
{
	struct tee_fs_rpc head = { 0 };
	TEE_Result res;
	int rc = -1;
	size_t len;

	DMSG("(%s)...", name);

	len = strlen(name) + 1;

	head.op = TEE_FS_RMDIR;

	res = sql_fs_send_cmd(&head, (void *)name, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

static int sql_fs_unlink_rpc(const char *file)
{
	struct tee_fs_rpc head = { 0 };
	size_t len = strlen(file) + 1;
	TEE_Result res;
	int rc = -1;

	DMSG("(%s)...", file);

	head.op = TEE_FS_UNLINK;

	res = sql_fs_send_cmd(&head, (void *)file, len, TEE_FS_MODE_IN);
	if (res != TEE_SUCCESS)
		goto exit;

	rc = head.res;
exit:
	DMSG("...%d", rc);
	return rc;
}

/*
 * End of interface with tee-supplicant
 */

static size_t meta_size(void)
{
	return tee_fs_get_header_size(META_FILE) +
	       sizeof(struct sql_fs_file_meta);
}

static size_t block_header_size(void)
{
	return tee_fs_get_header_size(BLOCK_FILE);
}

static size_t block_size_raw(void)
{
	return block_header_size() + BLOCK_SIZE;
}

/* Return the block number from a position in the user data */
static ssize_t block_num(tee_fs_off_t pos)
{
	return pos / BLOCK_SIZE;
}

/* Return the block number from a position in the DB file */
static ssize_t block_num_raw(tee_fs_off_t raw_pos)
{
	return (raw_pos - meta_size()) / block_size_raw();
}

/* Retur the position of a block in the DB file */
static ssize_t block_pos_raw(size_t block_num)
{
	return meta_size() + block_num * block_size_raw();
}

/* Given a position in the user data, return the offset in the DB file */
static tee_fs_off_t pos_to_raw(tee_fs_off_t pos)
{
	tee_fs_off_t res;

	if (pos < 0)
		return -1;
	res = meta_size() + block_num(pos) * block_size_raw();
	if (pos % BLOCK_SIZE) {
		res += block_header_size();
		res += pos % BLOCK_SIZE;
	}

	return res;
}

/* Given a position in the DB file, return the offset in the user data */
static tee_fs_off_t raw_to_pos(tee_fs_off_t raw_pos)
{
	tee_fs_off_t pos = raw_pos;
	ssize_t n = block_num_raw(raw_pos);

	if (n < 0)
		return -1;

	pos -= meta_size();
	pos -= block_header_size();
	if (pos < 0)
		return -1;

	return (n * BLOCK_SIZE) + (pos % BLOCK_SIZE);
}

static void put_fdp(struct sql_fs_fd *fdp)
{
	handle_put(&fs_db, fdp->fd);
	free(fdp);
}

#ifndef CFG_ENC_FS
static void copy_data(enum tee_fs_file_type type, uint8_t *raw_block,
		      const uint8_t *data, size_t len)
{
	size_t header_size = tee_fs_get_header_size(type);

	assert(type == META_FILE || type == BLOCK_FILE);

	memset(raw_block, 0xFF, header_size);
	memcpy(raw_block + header_size, data, len);
}
#endif

static int write_meta(TEE_Result *errno, struct sql_fs_fd *fdp)
{
	int fd = fdp->fd;
	size_t ct_size = meta_size();
	uint8_t *ct;
	int rc = -1;

	*errno = TEE_ERROR_GENERIC;

	ct = malloc(ct_size);
	if (!ct) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit;
	}

	rc = sql_fs_lseek_rpc(fd, 0, TEE_FS_SEEK_SET);
	if (rc < 0)
		goto exit;

#ifdef CFG_ENC_FS
	{
		TEE_Result res;

		res = tee_fs_encrypt_file(META_FILE,
					  (const uint8_t *)&fdp->meta,
					  sizeof(fdp->meta), ct, &ct_size,
					  fdp->encrypted_fek);
		if (res != TEE_SUCCESS) {
			*errno = res;
			rc = -1;
			goto exit;
		}
	}
#else
	copy_data(META_FILE, ct, (const uint8_t *)&fdp->meta,
		  sizeof(fdp->meta));
#endif

	rc = sql_fs_write_rpc(fdp->fd, ct, ct_size);
	if (rc != (int)ct_size)
		rc = -1;
	else
		rc = 0;

exit:
	free(ct);
	return rc;
}

static int create_meta(TEE_Result *errno, struct sql_fs_fd *fdp)
{
	TEE_Result res;

	memset(&fdp->meta, 0, sizeof(fdp->meta));

	res = tee_fs_generate_fek(fdp->encrypted_fek, TEE_FS_KM_FEK_SIZE);
	if (res != TEE_SUCCESS)
		return -1;

	return write_meta(errno, fdp);
}

/*
 * Read metadata block from disk, possibly create it if it does not exist and
 * and open flags allow
 */
static int read_meta(TEE_Result *errno, struct sql_fs_fd *fdp)
{
	size_t msize = meta_size();
	size_t out_size = sizeof(fdp->meta);
	uint8_t *meta = NULL;
	int rc = -1;

	*errno = TEE_ERROR_GENERIC;

	meta = malloc(msize);
	if (!meta) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit;
	}

	rc = sql_fs_lseek_rpc(fdp->fd, 0, TEE_FS_SEEK_SET);
	if (rc < 0)
		goto exit;

	rc = sql_fs_read_rpc(fdp->fd, meta, msize);
	if (rc < 0) {
		/* Read error */
		goto exit;
	} else if (rc == 0) {
		/* No meta data on disk yet */
		if (!(fdp->flags & TEE_FS_O_CREATE))
			goto exit;
		rc = create_meta(errno, fdp);
	} else if (rc == (int)msize) {
#ifdef CFG_ENC_FS
		TEE_Result res;

		res = tee_fs_decrypt_file(META_FILE, meta, msize,
					  (uint8_t *)&fdp->meta, &out_size,
					  fdp->encrypted_fek);
		if (res != TEE_SUCCESS) {
			*errno = res;
			rc = -1;
			goto exit;
		}
#else
		memcpy((uint8_t *)&fdp->meta,
		       meta + tee_fs_get_header_size(META_FILE),
		       out_size);
#endif
		rc = 0;
	} else {
		/* Unexpected data length */
		rc = -1;
	}
exit:
	free(meta);
	return rc;
}


/*
 * Read one block of user data.
 * Returns:
 *  < 0: read error
 *    0: block does not exist (reading past last block)
 *  > 0: success
 */
static int read_block(TEE_Result *errno, struct sql_fs_fd *fdp, size_t bnum,
		      uint8_t *data)
{
	size_t ct_size = block_size_raw();
	size_t out_size = BLOCK_SIZE;
	ssize_t pos = block_pos_raw(bnum);
	uint8_t *ct = NULL;
	int rc = -1;

	ct = malloc(ct_size);
	if (!ct) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit;
	}

	rc = sql_fs_lseek_rpc(fdp->fd, pos, TEE_FS_SEEK_SET);
	if (rc < 0) {
		*errno = TEE_ERROR_GENERIC;
		goto exit;
	}

	rc = sql_fs_read_rpc(fdp->fd, ct, ct_size);
	if (rc < 0) {
		*errno = TEE_ERROR_GENERIC;
		goto exit;
	}
	if (rc == 0) {
		/* Block does not exist */
		goto exit;
	}

#ifdef CFG_ENC_FS
	{
		TEE_Result res;

		res = tee_fs_decrypt_file(BLOCK_FILE, ct, ct_size, data,
					  &out_size, fdp->encrypted_fek);
		if (res != TEE_SUCCESS) {
			*errno = res;
			rc = -1;
			goto exit;
		}
	}
#else
	memcpy(data, ct + tee_fs_get_header_size(BLOCK_FILE), out_size);
#endif

exit:
	free(ct);
	return rc;
}

/* Write one block of user data */
static int write_block(TEE_Result *errno, struct sql_fs_fd *fdp,
		       size_t bnum, uint8_t *data)
{
	size_t ct_size = block_size_raw();
	ssize_t pos = block_pos_raw(bnum);
	uint8_t *ct = NULL;
	int rc = -1;

	ct = malloc(ct_size);
	if (!ct) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit;
	}

	rc = sql_fs_lseek_rpc(fdp->fd, pos, TEE_FS_SEEK_SET);
	if (rc < 0) {
		*errno = TEE_ERROR_GENERIC;
		goto exit;
	}

#ifdef CFG_ENC_FS
	{
		TEE_Result res;

		res = tee_fs_encrypt_file(BLOCK_FILE, data, BLOCK_SIZE, ct,
					  &ct_size, fdp->encrypted_fek);
		if (res != TEE_SUCCESS) {
			*errno = res;
			rc = -1;
			goto exit;
		}
	}
#else
	copy_data(BLOCK_FILE, ct, data, BLOCK_SIZE);
#endif

	rc = sql_fs_write_rpc(fdp->fd, ct, ct_size);
	if (rc < 0) {
		*errno = TEE_ERROR_GENERIC;
		goto exit;
	}

exit:
	free(ct);
	return rc;
}

/* Partial write (< BLOCK_SIZE) into a block: read/update/write */
static int write_block_partial(TEE_Result *errno, struct sql_fs_fd *fdp,
			       size_t bnum, const uint8_t *data, size_t len,
			       size_t offset)
{
	size_t buf_size = BLOCK_SIZE;
	uint8_t *buf = NULL;
	int rc = -1;

	if ((offset >= buf_size) || (offset + len > buf_size)) {
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit;
	}

	buf = malloc(buf_size);
	if (!buf) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit;
	}

	rc = read_block(errno, fdp, bnum, buf);
	if (rc < 0)
		goto exit;

	memcpy(buf + offset, data, len);

	rc = write_block(errno, fdp, bnum, buf);

exit:
	free(buf);
	return rc;
}

static int sql_fs_ftruncate(TEE_Result *errno, int fd, tee_fs_off_t new_length)
{
	struct sql_fs_fd *fdp = handle_lookup(&fs_db, fd);
	tee_fs_off_t old_length;
	uint8_t *buf = NULL;
	int rc = -1;

	DMSG("(fd: %d, new_length: %" PRId64 ")...", fd, new_length);

	*errno = TEE_ERROR_GENERIC;

	if (!fdp) {
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit_ret;
	}

	old_length = (tee_fs_off_t)fdp->meta.length;

	if (new_length == old_length) {
		rc = 0;
		goto exit_ret;
	}

	sql_fs_begin_transaction_rpc();

	if (new_length < old_length) {
		/* Trim unused blocks */
		int old_last_block = block_num(old_length);
		int last_block = block_num(new_length);
		tee_fs_off_t off;

		if (last_block < old_last_block) {
			off = block_pos_raw(last_block);
			rc = sql_fs_ftruncate_rpc(fd, off);
			if (rc < 0)
				goto exit;
		}
	} else {
		/* Extend file with zeroes */
		tee_fs_off_t off = old_length % BLOCK_SIZE;
		size_t bnum = block_num(old_length);
		size_t end_bnum = block_num(new_length);

		buf = calloc(1, BLOCK_SIZE);
		if (!buf) {
			*errno = TEE_ERROR_OUT_OF_MEMORY;
			goto exit;
		}

		while (bnum <= end_bnum) {
			size_t len = (size_t)BLOCK_SIZE - (size_t)off;

			rc = write_block_partial(errno, fdp, bnum, buf, len,
						 off);
			if (rc < 0)
				goto exit;
			off = 0;
			bnum++;
		}
	}

	fdp->meta.length = new_length;
	rc = write_meta(errno, fdp);

exit:
	sql_fs_end_transaction_rpc(rc < 0);
	free(buf);
exit_ret:
	DMSG("...%d", rc);
	return rc;
}

static tee_fs_off_t sql_fs_lseek(TEE_Result *errno, int fd,
				 tee_fs_off_t offset, int whence)
{
	struct sql_fs_fd *fdp = handle_lookup(&fs_db, fd);
	tee_fs_off_t ret = -1;
	tee_fs_off_t raw_pos;
	tee_fs_off_t pos;

	DMSG("(fd: %d, offset: %" PRId64 ", whence: %d)...", fd, offset,
	     whence);

	*errno = TEE_ERROR_GENERIC;

	if (!fdp)
		goto exit_ret;

	sql_fs_begin_transaction_rpc();

	switch (whence) {
	case TEE_FS_SEEK_SET:
		pos = offset;
		break;

	case TEE_FS_SEEK_CUR:
		pos = fdp->pos + offset;
		break;

	case TEE_FS_SEEK_END:
		pos = fdp->meta.length + offset;
		break;

	default:
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit;
	}

	raw_pos = pos_to_raw(pos);
	if (raw_pos < 0)
		goto exit;
	raw_pos = sql_fs_lseek_rpc(fd, raw_pos, TEE_FS_SEEK_SET);
	if (raw_pos < 0)
		goto exit;
	ret = raw_to_pos(raw_pos);
	if (ret < 0)
		goto exit;

exit:
	sql_fs_end_transaction_rpc(ret < 0);
	fdp->pos = ret;
exit_ret:
	DMSG("...%" PRId64, ret);
	return ret;
}

static int sql_fs_close(int fd)
{
	struct sql_fs_fd *fdp = handle_lookup(&fs_db, fd);
	int ret = -1;

	DMSG("(fd: %d)...", fd);

	if (!fdp)
		goto exit;

	sql_fs_close_rpc(fd);
	put_fdp(fdp);

	ret = 0;
exit:
	DMSG("...%d", ret);
	return ret;
}

static int sql_fs_open(TEE_Result *errno, const char *file, int flags, ...)
{
	struct sql_fs_fd *fdp = NULL;
	int rflags = flags | TEE_FS_O_RDWR; /* Need to read/write meta */
	int exists;
	int fd = -1;

	DMSG("(file: %s, flags: %d)...", file, flags);

	*errno = TEE_ERROR_GENERIC;

	exists = !sql_fs_access_rpc(file, TEE_FS_F_OK);
	if (flags & TEE_FS_O_CREATE) {
		if ((flags & TEE_FS_O_EXCL) && exists) {
			*errno = TEE_ERROR_ACCESS_CONFLICT;
			goto exit;
		}
	} else {
		if (!exists) {
			*errno = TEE_ERROR_ITEM_NOT_FOUND;
			goto exit;
		}
	}

	fdp = (struct sql_fs_fd *)calloc(1, sizeof(*fdp));
	if (!fdp) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit;
	}

	fdp->fd = tee_fs_rpc_open(OPTEE_MSG_RPC_CMD_SQL_FS, file, rflags);
	if (fdp->fd < 0)
		goto exit;

	fdp->flags = flags;

	fd = read_meta(errno, fdp);
	if (fd < 0)
		goto exit;

	fd = handle_get(&fs_db, fdp);

exit:
	if (fd < 0)
		free(fdp);
	DMSG("...%d", fd);
	return fd;
}

static int sql_fs_read(TEE_Result *errno, int fd, void *buf, size_t len)
{
	struct sql_fs_fd *fdp = handle_lookup(&fs_db, fd);
	size_t remain_bytes = len;
	uint8_t *data_ptr = buf;
	uint8_t *block = NULL;
	int start_block_num;
	int end_block_num;
	int res = -1;
	int ret;

	DMSG("(fd: %d, buf: %p, len: %zu)...", fd, (void *)buf, len);

	*errno = TEE_ERROR_GENERIC;

	if (!fdp) {
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit_ret;
	}

	if (fdp->pos + len > fdp->meta.length)
		len = fdp->meta.length - fdp->pos;

	if (!len) {
		res = 0;
		goto exit_ret;
	}

	if (!buf) {
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit_ret;
	}

	if (fdp->flags & TEE_FS_O_WRONLY) {
		*errno = TEE_ERROR_ACCESS_CONFLICT;
		goto exit_ret;
	}

	start_block_num = block_num(fdp->pos);
	end_block_num = block_num(fdp->pos + len - 1);

	block = malloc(BLOCK_SIZE);
	if (!block) {
		*errno = TEE_ERROR_OUT_OF_MEMORY;
		goto exit_ret;
	}

	sql_fs_begin_transaction_rpc();

	while (start_block_num <= end_block_num) {
		tee_fs_off_t offset = fdp->pos % BLOCK_SIZE;
		size_t size_to_read = MIN(remain_bytes, (size_t)BLOCK_SIZE);

		if (size_to_read + offset > BLOCK_SIZE)
			size_to_read = BLOCK_SIZE - offset;

		/*
		 * REVISIT: implement read_block_partial() since we have
		 * write_block_partial()?
		 */
		res = read_block(errno, fdp, start_block_num, block);
		if (res < 0)
			goto exit;

		memcpy(data_ptr, block + offset, size_to_read);

		data_ptr += size_to_read;
		remain_bytes -= size_to_read;
		fdp->pos += size_to_read;

		start_block_num++;
	}
	res = 0;
exit:
	sql_fs_end_transaction_rpc(res < 0);
	free(block);
exit_ret:
	ret = (res < 0) ? res : (int)len;
	DMSG("...%d", ret);
	return ret;
}

static int sql_fs_write(TEE_Result *errno, int fd, const void *buf, size_t len)
{
	struct sql_fs_fd *fdp = handle_lookup(&fs_db, fd);
	size_t remain_bytes = len;
	const uint8_t *data_ptr = buf;
	int start_block_num;
	int end_block_num;
	int res = -1;
	int ret;

	DMSG("(fd: %d, buf: %p, len: %zu)...", fd, (void *)buf, len);

	*errno = TEE_ERROR_GENERIC;

	if (!fdp) {
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit_ret;
	}

	if (!len) {
		res = 0;
		goto exit_ret;
	}

	if (!buf) {
		*errno = TEE_ERROR_BAD_PARAMETERS;
		goto exit_ret;
	}

	if (fdp->flags & TEE_FS_O_RDONLY) {
		*errno = TEE_ERROR_ACCESS_CONFLICT;
		goto exit_ret;
	}

	sql_fs_begin_transaction_rpc();

	if (fdp->meta.length < (size_t)fdp->pos) {
		/* Fill hole */
		res = sql_fs_ftruncate(errno, fd, fdp->pos);
		if (res < 0)
			goto exit;
	}

	start_block_num = block_num(fdp->pos);
	end_block_num = block_num(fdp->pos + len - 1);

	while (start_block_num <= end_block_num) {
		tee_fs_off_t offset = fdp->pos % BLOCK_SIZE;
		size_t size_to_write = MIN(remain_bytes, (size_t)BLOCK_SIZE);

		if (size_to_write + offset > BLOCK_SIZE)
			size_to_write = BLOCK_SIZE - offset;

		res = write_block_partial(errno, fdp, start_block_num,
					  data_ptr, size_to_write, offset);
		if (res < 0)
			goto exit;

		data_ptr += size_to_write;
		remain_bytes -= size_to_write;
		fdp->pos += size_to_write;

		start_block_num++;
	}

	fdp->meta.length = fdp->pos;
	res = write_meta(errno, fdp);
exit:
	sql_fs_end_transaction_rpc(res < 0);
exit_ret:
	ret = (res < 0) ? res : (int)len;
	DMSG("...%d", ret);
	return ret;
}

const struct tee_file_operations sql_fs_ops = {
	.open = sql_fs_open,
	.close = sql_fs_close,
	.read = sql_fs_read,
	.write = sql_fs_write,
	.lseek = sql_fs_lseek,
	.ftruncate = sql_fs_ftruncate,

	.access = sql_fs_access_rpc,
	.opendir = sql_fs_opendir_rpc,
	.closedir = sql_fs_closedir_rpc,
	.readdir = sql_fs_readdir_rpc,
	.mkdir = sql_fs_mkdir_rpc,
	.rmdir = sql_fs_rmdir_rpc,
	.rename = sql_fs_rename_rpc,
	.unlink = sql_fs_unlink_rpc,
};
