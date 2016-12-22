#Crypto enable
CFG_CRYPTO ?= y

ifeq (y,$(CFG_CRYPTO))

# Ciphers
CFG_CRYPTO_AES ?= y
CFG_CRYPTO_DES ?= y

# Cipher block modes
CFG_CRYPTO_ECB ?= y
CFG_CRYPTO_CBC ?= y
CFG_CRYPTO_CTR ?= y
CFG_CRYPTO_CTS ?= y
CFG_CRYPTO_XTS ?= y

# Message authentication codes
CFG_CRYPTO_HMAC ?= y
CFG_CRYPTO_CMAC ?= y
CFG_CRYPTO_CBC_MAC ?= y

# Hashes
CFG_CRYPTO_MD5 ?= y
CFG_CRYPTO_SHA1 ?= y
CFG_CRYPTO_SHA224 ?= y
CFG_CRYPTO_SHA256 ?= y
CFG_CRYPTO_SHA384 ?= y
CFG_CRYPTO_SHA512 ?= y

# Asymmetric ciphers
CFG_CRYPTO_DSA ?= y
CFG_CRYPTO_RSA ?= y
CFG_CRYPTO_DH ?= y
CFG_CRYPTO_ECC ?= y

# Authenticated encryption
CFG_CRYPTO_CCM ?= y
CFG_CRYPTO_GCM ?= y

endif

ifeq ($(CFG_WITH_PAGER),y)
ifneq ($(CFG_CRYPTO_SHA256),y)
ifneq ($(CFG_CRYPTO_SHA256_ARM32_CE),y)
$(warning Warning: Enabling CFG_CRYPTO_SHA256 [required by CFG_WITH_PAGER])
CFG_CRYPTO_SHA256:=y
endif
endif
endif

cryp-enable-all-depends = $(call cfg-enable-all-depends,$(strip $(1)),$(foreach v,$(2),CFG_CRYPTO_$(v)))
$(eval $(call cryp-enable-all-depends,CFG_ENC_FS, AES ECB CTR HMAC SHA256 GCM))

# Dependency checks: warn and disable some features if dependencies are not met

cryp-dep-one = $(call cfg-depends-one,CFG_CRYPTO_$(strip $(1)),$(patsubst %, CFG_CRYPTO_%,$(strip $(2))))
cryp-dep-all = $(call cfg-depends-all,CFG_CRYPTO_$(strip $(1)),$(patsubst %, CFG_CRYPTO_%,$(strip $(2))))

$(eval $(call cryp-dep-one, ECB, AES DES))
$(eval $(call cryp-dep-one, CBC, AES DES))
$(eval $(call cryp-dep-one, CTR, AES))
# CTS is implemented with ECB and CBC
$(eval $(call cryp-dep-all, CTS, AES ECB CBC))
$(eval $(call cryp-dep-one, XTS, AES))
$(eval $(call cryp-dep-one, HMAC, AES DES))
$(eval $(call cryp-dep-one, HMAC, MD5 SHA1 SHA224 SHA256 SHA384 SHA512))
$(eval $(call cryp-dep-one, CMAC, AES))
$(eval $(call cryp-dep-one, CBC_MAC, AES DES))
$(eval $(call cryp-dep-one, CCM, AES))
$(eval $(call cryp-dep-one, GCM, AES))
# If no AES cipher mode is left, disable AES
$(eval $(call cryp-dep-one, AES, ECB CBC CTR CTS XTS))
# If no DES cipher mode is left, disable DES
$(eval $(call cryp-dep-one, DES, ECB CBC))

cryp-one-enabled = $(call cfg-one-enabled,$(foreach v,$(1),CFG_CRYPTO_$(v)))
cryp-all-enabled = $(call cfg-all-enabled,$(foreach v,$(1),CFG_CRYPTO_$(v)))

_CFG_CRYPTO_WITH_ACIPHER := $(call cryp-one-enabled, RSA DSA DH ECC)
_CFG_CRYPTO_WITH_AUTHENC := $(and $(filter y,$(CFG_CRYPTO_AES)), $(call cryp-one-enabled, CCM GCM))
_CFG_CRYPTO_WITH_CIPHER := $(call cryp-one-enabled, AES DES)
_CFG_CRYPTO_WITH_HASH := $(call cryp-one-enabled, MD5 SHA1 SHA224 SHA256 SHA384 SHA512)
_CFG_CRYPTO_WITH_MAC := $(call cryp-one-enabled, HMAC CMAC CBC_MAC)
_CFG_CRYPTO_WITH_CBC := $(call cryp-one-enabled, CBC CBC_MAC)
_CFG_CRYPTO_WITH_ASN1 := $(call cryp-one-enabled, RSA DSA)
_CFG_CRYPTO_WITH_FORTUNA_PRNG := $(call cryp-all-enabled, AES SHA256)

cppflags-lib-$(libtomcrypt_with_optimize_size) += -DLTC_SMALL_CODE -DLTC_NO_FAST

# Get mpa.h which normally is an internal .h file
global-incdirs-y += ../libtomcrypt/include
subdirs-y += ../libtomcrypt/src/math
subdirs-y += ../libtomcrypt/src/misc
subdirs-y += ../libtomcrypt/src/pk/ecc
subdirs-y += ../libtomcrypt/src/pk/rsa
srcs-y += ../libtomcrypt/src/mpa_desc.c
cflags-../libtomcrypt/src/mpa_desc.c-y += -Wno-declaration-after-statement
cflags-../libtomcrypt/src/mpa_desc.c-y += -Wno-unused-parameter

ifeq ($(CFG_CRYPT_ENABLE_CEPKA),y)
# Provider fro Crypto Engine PKA
srcs-y += tee_pka_provider.c
cflags-tee_pka_provider.c-y += -Wno-maybe-uninitialized
endif

#Provider for Crypto Engine Secure
srcs-y += tee_ss_provider.c
cflags-tee_ss_provider.c-y += -Wno-unused-function
cflags-tee_ss_provider.c-y += -Wno-int-to-pointer-cast
cflags-tee_ss_provider.c-y += -Wno-strict-aliasing
cflags-tee_ss_provider.c-y += -Wno-pedantic
cflags-tee_ss_provider.c-y += -Wno-undef -Wno-maybe-uninitialized

# Common Function for Provider
srcs-y += tee_provider_common.c