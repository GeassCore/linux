// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */

#include <errno.h>
#include <linux/if_alg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int open_alg(const char *type, const char *name)
{
	struct sockaddr_alg sa = {
		.salg_family = AF_ALG,
	};
	int fd;

	if (snprintf((char *)sa.salg_type, sizeof(sa.salg_type), "%s", type) >=
	    (int)sizeof(sa.salg_type) ||
	    snprintf((char *)sa.salg_name, sizeof(sa.salg_name), "%s", name) >=
	    (int)sizeof(sa.salg_name)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		close(fd);
		return -1;
	}

	return fd;
}

static int read_all(int fd, uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = read(fd, buf + done, len - done);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		done += ret;
	}

	return 0;
}

static int skcipher(const char *name, const uint8_t *key, size_t keylen,
		    const uint8_t *iv, size_t ivlen, const uint8_t *input,
		    uint8_t *output, size_t len, uint32_t operation)
{
	union {
		struct cmsghdr align;
		uint8_t data[CMSG_SPACE(sizeof(uint32_t)) +
			     CMSG_SPACE(sizeof(struct af_alg_iv) + 16)];
	} control = { 0 };
	struct cmsghdr *cmsg;
	struct msghdr msg = { 0 };
	struct iovec iov = {
		.iov_base = (void *)input,
		.iov_len = len,
	};
	struct af_alg_iv *alg_iv;
	int algfd = -1;
	int opfd = -1;
	int ret = -1;

	if (ivlen > 16) {
		errno = EINVAL;
		return -1;
	}

	algfd = open_alg("skcipher", name);
	if (algfd < 0)
		goto out;
	if (setsockopt(algfd, SOL_ALG, ALG_SET_KEY, key, keylen))
		goto out;

	opfd = accept(algfd, NULL, NULL);
	if (opfd < 0)
		goto out;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.data;
	msg.msg_controllen = CMSG_SPACE(sizeof(uint32_t));
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_ALG;
	cmsg->cmsg_type = ALG_SET_OP;
	cmsg->cmsg_len = CMSG_LEN(sizeof(uint32_t));
	memcpy(CMSG_DATA(cmsg), &operation, sizeof(operation));

	if (ivlen) {
		msg.msg_controllen += CMSG_SPACE(sizeof(*alg_iv) + ivlen);
		cmsg = CMSG_NXTHDR(&msg, cmsg);
		cmsg->cmsg_level = SOL_ALG;
		cmsg->cmsg_type = ALG_SET_IV;
		cmsg->cmsg_len = CMSG_LEN(sizeof(*alg_iv) + ivlen);
		alg_iv = (void *)CMSG_DATA(cmsg);
		alg_iv->ivlen = ivlen;
		memcpy(alg_iv->iv, iv, ivlen);
	}

	if (sendmsg(opfd, &msg, 0) != (ssize_t)len)
		goto out;
	if (read_all(opfd, output, len))
		goto out;
	ret = 0;
out:
	if (opfd >= 0)
		close(opfd);
	if (algfd >= 0)
		close(algfd);
	return ret;
}

static int hash(const char *name, const uint8_t *input, size_t inputlen,
		uint8_t *output, size_t outputlen)
{
	int algfd = -1;
	int opfd = -1;
	int ret = -1;

	algfd = open_alg("hash", name);
	if (algfd < 0)
		goto out;
	opfd = accept(algfd, NULL, NULL);
	if (opfd < 0)
		goto out;
	if (write(opfd, input, inputlen) != (ssize_t)inputlen)
		goto out;
	if (read_all(opfd, output, outputlen))
		goto out;
	ret = 0;
out:
	if (opfd >= 0)
		close(opfd);
	if (algfd >= 0)
		close(algfd);
	return ret;
}

static int check_skcipher(const char *name, const uint8_t *key, size_t keylen,
			  const uint8_t *iv, size_t ivlen,
			  const uint8_t *plain, const uint8_t *cipher,
			  size_t len)
{
	uint8_t output[64];
	int ret;

	if (len > sizeof(output))
		return -1;
	errno = 0;
	ret = skcipher(name, key, keylen, iv, ivlen, plain, output, len,
			ALG_OP_ENCRYPT);
	if (ret || memcmp(output, cipher, len)) {
		fprintf(stderr, "FAIL: %s encryption: %s\n", name,
			ret ? strerror(errno) : "wrong result");
		return -1;
	}
	errno = 0;
	ret = skcipher(name, key, keylen, iv, ivlen, cipher, output, len,
			ALG_OP_DECRYPT);
	if (ret || memcmp(output, plain, len)) {
		fprintf(stderr, "FAIL: %s decryption: %s\n", name,
			ret ? strerror(errno) : "wrong result");
		return -1;
	}
	printf("PASS: %s encrypt/decrypt KAT\n", name);
	return 0;
}

static int check_hash(const char *name, const uint8_t *input, size_t inputlen,
		      const uint8_t *expected, size_t len)
{
	uint8_t output[64];
	int ret;

	if (len > sizeof(output))
		return -1;
	errno = 0;
	ret = hash(name, input, inputlen, output, len);
	if (ret || memcmp(output, expected, len)) {
		size_t i;

		fprintf(stderr, "FAIL: %s KAT (%zu-byte input): %s\n", name,
			inputlen, ret ? strerror(errno) : "wrong result");
		if (!ret) {
			fprintf(stderr, " actual: ");
			for (i = 0; i < len; i++)
				fprintf(stderr, "%02x", output[i]);
			fprintf(stderr, "\nexpected: ");
			for (i = 0; i < len; i++)
				fprintf(stderr, "%02x", expected[i]);
			fprintf(stderr, "\n");
		}
		return -1;
	}
	printf("PASS: %s KAT (%zu-byte input)\n", name, inputlen);
	return 0;
}

int main(void)
{
	static const uint8_t hash_input[] = "abcd";
	static const uint8_t unaligned_hash_input[] = "abc";
	static const uint8_t ecb_key[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const uint8_t ecb_plain[] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	static const uint8_t ecb_cipher[] = {
		0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
		0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
	};
	static const uint8_t cbc_key[] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
	};
	static const uint8_t cbc_iv[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const uint8_t cbc_plain[] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
		0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	};
	static const uint8_t cbc_cipher[] = {
		0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
		0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
	};
	static const uint8_t md5[] = {
		0xe2, 0xfc, 0x71, 0x4c, 0x47, 0x27, 0xee, 0x93,
		0x95, 0xf3, 0x24, 0xcd, 0x2e, 0x7f, 0x33, 0x1f,
	};
	static const uint8_t sha1[] = {
		0x81, 0xfe, 0x8b, 0xfe, 0x87, 0x57, 0x6c, 0x3e,
		0xcb, 0x22, 0x42, 0x6f, 0x8e, 0x57, 0x84, 0x73,
		0x82, 0x91, 0x7a, 0xcf,
	};
	static const uint8_t sha224[] = {
		0xa7, 0x66, 0x54, 0xd8, 0xe3, 0x55, 0x0e, 0x9a,
		0x2d, 0x67, 0xa0, 0xee, 0xb6, 0xc6, 0x7b, 0x22,
		0x0e, 0x58, 0x85, 0xed, 0xdd, 0x3f, 0xde, 0x13,
		0x58, 0x06, 0xe6, 0x01,
	};
	static const uint8_t sha256[] = {
		0x88, 0xd4, 0x26, 0x6f, 0xd4, 0xe6, 0x33, 0x8d,
		0x13, 0xb8, 0x45, 0xfc, 0xf2, 0x89, 0x57, 0x9d,
		0x20, 0x9c, 0x89, 0x78, 0x23, 0xb9, 0x21, 0x7d,
		0xa3, 0xe1, 0x61, 0x93, 0x6f, 0x03, 0x15, 0x89,
	};
	static const uint8_t sha256_unaligned[] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	static const uint8_t sha256_56_bytes[] = {
		0xb3, 0x54, 0x39, 0xa4, 0xac, 0x6f, 0x09, 0x48,
		0xb6, 0xd6, 0xf9, 0xe3, 0xc6, 0xaf, 0x0f, 0x5f,
		0x59, 0x0c, 0xe2, 0x0f, 0x1b, 0xde, 0x70, 0x90,
		0xef, 0x79, 0x70, 0x68, 0x6e, 0xc6, 0x73, 0x8a,
	};
	static const uint8_t sha256_64_bytes[] = {
		0xff, 0xe0, 0x54, 0xfe, 0x7a, 0xe0, 0xcb, 0x6d,
		0xc6, 0x5c, 0x3a, 0xf9, 0xb6, 0x1d, 0x52, 0x09,
		0xf4, 0x39, 0x85, 0x1d, 0xb4, 0x3d, 0x0b, 0xa5,
		0x99, 0x73, 0x37, 0xdf, 0x15, 0x46, 0x68, 0xeb,
	};
	static const uint8_t sha384[] = {
		0x11, 0x65, 0xb3, 0x40, 0x6f, 0xf0, 0xb5, 0x2a,
		0x3d, 0x24, 0x72, 0x1f, 0x78, 0x54, 0x62, 0xca,
		0x22, 0x76, 0xc9, 0xf4, 0x54, 0xa1, 0x16, 0xc2,
		0xb2, 0xba, 0x20, 0x17, 0x1a, 0x79, 0x05, 0xea,
		0x5a, 0x02, 0x66, 0x82, 0xeb, 0x65, 0x9c, 0x4d,
		0x5f, 0x11, 0x5c, 0x36, 0x3a, 0xa3, 0xc7, 0x9b,
	};
	static const uint8_t sha512[] = {
		0xd8, 0x02, 0x2f, 0x20, 0x60, 0xad, 0x6e, 0xfd,
		0x29, 0x7a, 0xb7, 0x3d, 0xcc, 0x53, 0x55, 0xc9,
		0xb2, 0x14, 0x05, 0x4b, 0x0d, 0x17, 0x76, 0xa1,
		0x36, 0xa6, 0x69, 0xd2, 0x6a, 0x7d, 0x3b, 0x14,
		0xf7, 0x3a, 0xa0, 0xd0, 0xeb, 0xff, 0x19, 0xee,
		0x33, 0x33, 0x68, 0xf0, 0x16, 0x4b, 0x64, 0x19,
		0xa9, 0x6d, 0xa4, 0x9e, 0x3e, 0x48, 0x17, 0x53,
		0xe7, 0xe9, 0x6b, 0x71, 0x6b, 0xdc, 0xcb, 0x6f,
	};
	uint8_t repeated_a[64];
	int ret = 0;

	memset(repeated_a, 'a', sizeof(repeated_a));

	ret |= check_skcipher("ecb(aes)", ecb_key, sizeof(ecb_key), NULL, 0,
			       ecb_plain, ecb_cipher, sizeof(ecb_plain));
	ret |= check_skcipher("cbc(aes)", cbc_key, sizeof(cbc_key), cbc_iv,
			       sizeof(cbc_iv), cbc_plain, cbc_cipher,
			       sizeof(cbc_plain));
	ret |= check_hash("md5", hash_input, sizeof(hash_input) - 1,
			  md5, sizeof(md5));
	ret |= check_hash("sha1", hash_input, sizeof(hash_input) - 1,
			  sha1, sizeof(sha1));
	ret |= check_hash("sha224", hash_input, sizeof(hash_input) - 1,
			  sha224, sizeof(sha224));
	ret |= check_hash("sha256", hash_input, sizeof(hash_input) - 1,
			  sha256, sizeof(sha256));
	ret |= check_hash("sha384", hash_input, sizeof(hash_input) - 1,
			  sha384, sizeof(sha384));
	ret |= check_hash("sha512", hash_input, sizeof(hash_input) - 1,
			  sha512, sizeof(sha512));
	ret |= check_hash("sha256", unaligned_hash_input,
			  sizeof(unaligned_hash_input) - 1, sha256_unaligned,
			  sizeof(sha256_unaligned));
	ret |= check_hash("sha256", repeated_a, 56, sha256_56_bytes,
			  sizeof(sha256_56_bytes));
	ret |= check_hash("sha256", repeated_a, sizeof(repeated_a),
			  sha256_64_bytes, sizeof(sha256_64_bytes));

	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
