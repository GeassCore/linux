// SPDX-License-Identifier: MIT
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/* Minimal PowerVR DRM UAPI smoke test; no libdrm dependency. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* This kernel tree annotates pointers in drm.h even for userspace includes. */
#ifndef __EXPORTED_HEADERS__
#define __EXPORTED_HEADERS__
#endif
#ifndef __user
#define __user
#endif

#include <drm/drm.h>
#include <drm/pvr_drm.h>

#define TEST_BO_SIZE 4096U

static int fail_errno(const char *operation)
{
	fprintf(stderr, "FAIL: %s: %s\n", operation, strerror(errno));
	return -1;
}

static int open_powervr_node(const char *requested, char *path, size_t path_size)
{
	unsigned int minor;

	if (requested) {
		int fd;

		if (snprintf(path, path_size, "%s", requested) >= (int)path_size) {
			errno = ENAMETOOLONG;
			return -1;
		}

		fd = open(path, O_RDWR | O_CLOEXEC);
		return fd;
	}

	for (minor = 128; minor < 192; minor++) {
		int fd;

		snprintf(path, path_size, "/dev/dri/renderD%u", minor);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}

	errno = ENODEV;
	return -1;
}

static int check_driver(int fd)
{
	char name[32] = { 0 };
	char date[32] = { 0 };
	char desc[128] = { 0 };
	struct drm_version version = {
		.name_len = sizeof(name) - 1,
		.name = name,
		.date_len = sizeof(date) - 1,
		.date = date,
		.desc_len = sizeof(desc) - 1,
		.desc = desc,
	};

	if (ioctl(fd, DRM_IOCTL_VERSION, &version))
		return fail_errno("DRM_IOCTL_VERSION");

	if (strcmp(name, "powervr")) {
		fprintf(stderr, "FAIL: expected powervr driver, got '%s'\n", name);
		return -1;
	}

	printf("PASS: driver=%s version=%d.%d.%d date=%s desc=%s\n",
	       name, version.version_major, version.version_minor,
	       version.version_patchlevel, date, desc);
	return 0;
}

static int dev_query(int fd, uint32_t type, void *data, uint32_t size)
{
	struct drm_pvr_ioctl_dev_query_args args = {
		.type = type,
		.size = size,
		.pointer = (uintptr_t)data,
	};

	return ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &args);
}

static int test_queries(int fd)
{
	struct drm_pvr_dev_query_gpu_info gpu = { 0 };
	struct drm_pvr_dev_query_runtime_info runtime = { 0 };
	struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT] = { 0 };
	struct drm_pvr_dev_query_heap_info heap_info = {
		.heaps = DRM_PVR_OBJ_ARRAY(DRM_PVR_HEAP_COUNT, heaps),
	};
	unsigned int present_heaps = 0;
	unsigned int i;

	if (dev_query(fd, DRM_PVR_DEV_QUERY_GPU_INFO_GET, &gpu, sizeof(gpu)))
		return fail_errno("GPU_INFO DEV_QUERY");

	printf("PASS: GPU BVNC=%" PRIu64 ".%" PRIu64 ".%" PRIu64 ".%" PRIu64
	       " phantoms=%u\n",
	       (uint64_t)((gpu.gpu_id >> 48) & 0xffff),
	       (uint64_t)((gpu.gpu_id >> 32) & 0xffff),
	       (uint64_t)((gpu.gpu_id >> 16) & 0xffff),
	       (uint64_t)(gpu.gpu_id & 0xffff),
	       gpu.num_phantoms);

	if (dev_query(fd, DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET, &runtime,
		      sizeof(runtime)))
		return fail_errno("RUNTIME_INFO DEV_QUERY");

	if (!runtime.free_list_min_pages || !runtime.free_list_max_pages) {
		fprintf(stderr, "FAIL: invalid runtime free-list limits\n");
		return -1;
	}

	printf("PASS: runtime free-list pages min=%" PRIu64 " max=%" PRIu64
	       " max_coeffs=%u\n",
	       (uint64_t)runtime.free_list_min_pages,
	       (uint64_t)runtime.free_list_max_pages,
	       runtime.max_coeffs);

	if (dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET, &heap_info,
		      sizeof(heap_info)))
		return fail_errno("HEAP_INFO DEV_QUERY");

	for (i = 0; i < heap_info.heaps.count; i++) {
		if (!heaps[i].size)
			continue;
		present_heaps++;
		printf("INFO: heap[%u] base=0x%" PRIx64 " size=0x%" PRIx64
		       " page_shift=%u\n", i, (uint64_t)heaps[i].base,
		       (uint64_t)heaps[i].size,
		       heaps[i].page_size_log2);
	}

	if (!present_heaps) {
		fprintf(stderr, "FAIL: driver returned no usable GPU heaps\n");
		return -1;
	}

	printf("PASS: queried %u GPU heaps, %u present\n",
	       heap_info.heaps.count, present_heaps);
	return 0;
}

static int test_bo(int fd)
{
	struct drm_pvr_ioctl_create_bo_args create = {
		.size = TEST_BO_SIZE,
		.flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS,
	};
	struct drm_pvr_ioctl_get_bo_mmap_offset_args offset = { 0 };
	struct drm_gem_close close_args = { 0 };
	volatile uint32_t *map = MAP_FAILED;
	int ret = -1;

	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &create))
		return fail_errno("DRM_IOCTL_PVR_CREATE_BO");

	offset.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &offset)) {
		fail_errno("DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET");
		goto out_close;
	}

	map = mmap(NULL, TEST_BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   offset.offset);
	if (map == MAP_FAILED) {
		fail_errno("mmap PowerVR BO");
		goto out_close;
	}

	map[0] = 0x5a17c0de;
	map[(TEST_BO_SIZE / sizeof(*map)) - 1] = 0xc001d00d;
	if (map[0] != 0x5a17c0de ||
	    map[(TEST_BO_SIZE / sizeof(*map)) - 1] != 0xc001d00d) {
		fprintf(stderr, "FAIL: BO CPU mapping readback mismatch\n");
		goto out_unmap;
	}

	printf("PASS: created, mmap'ed and verified %u-byte PowerVR BO\n",
	       TEST_BO_SIZE);
	ret = 0;

out_unmap:
	munmap((void *)map, TEST_BO_SIZE);
out_close:
	close_args.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_args)) {
		fail_errno("DRM_IOCTL_GEM_CLOSE");
		ret = -1;
	}
	return ret;
}

static int test_vm_context(int fd)
{
	struct drm_pvr_ioctl_create_vm_context_args create = { 0 };
	struct drm_pvr_ioctl_destroy_vm_context_args destroy = { 0 };

	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &create))
		return fail_errno("DRM_IOCTL_PVR_CREATE_VM_CONTEXT");

	destroy.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &destroy))
		return fail_errno("DRM_IOCTL_PVR_DESTROY_VM_CONTEXT");

	printf("PASS: created and destroyed PowerVR VM context\n");
	return 0;
}

int main(int argc, char **argv)
{
	char path[64];
	int fd;
	int ret = EXIT_FAILURE;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [/dev/dri/renderD*]\n", argv[0]);
		return EXIT_FAILURE;
	}

	fd = open_powervr_node(argc == 2 ? argv[1] : NULL, path, sizeof(path));
	if (fd < 0) {
		fail_errno("open PowerVR render node");
		return EXIT_FAILURE;
	}

	printf("INFO: testing %s\n", path);
	if (check_driver(fd) || test_queries(fd) || test_bo(fd) ||
	    test_vm_context(fd))
		goto out;

	printf("PASS: PowerVR DRM UAPI functional smoke test\n");
	ret = EXIT_SUCCESS;
out:
	close(fd);
	return ret;
}
