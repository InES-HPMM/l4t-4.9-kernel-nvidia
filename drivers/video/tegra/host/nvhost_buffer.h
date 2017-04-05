/*
 * NVHOST Buffer Management Header
 *
 * Copyright (c) 2016-2017, NVIDIA Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __NVHOST_NVHOST_BUFFER_H__
#define __NVHOST_NVHOST_BUFFER_H__

#include <linux/dma-buf.h>

/**
 * @brief		Information needed for buffers
 *
 * pdev			Pointer to NVHOST device
 * buffer_list		List of all the buffers used by a file pointer
 * buffer_list_mutex	Mutex for the buffer list
 * kref			Reference count for the bufferlist
 *
 */
struct nvhost_buffers {
	struct platform_device *pdev;
	struct list_head buffer_list;
	struct mutex buffer_list_mutex;
	struct kref kref;
};

/**
 * @brief			Initialize the nvhost_buffer per open request
 *
 * This function allocates nvhost_buffers struct and init the bufferlist
 * and mutex.
 *
 * @param nvhost_buffers	Pointer to nvhost_buffers struct
 * @return			nvhost_buffers pointer on success
 *					or negative on error
 *
 */
struct nvhost_buffers *nvhost_buffer_init(struct platform_device *pdev);

/**
 * @brief			Pin the memhandle using dma_buf functions
 *
 * This function maps the buffer memhandle list passed from user side
 * to device iova.
 *
 * @param nvhost_buffers	Pointer to nvhost_buffers struct
 * @param dmabufs		Pointer to dmabuffer list
 * @param count			Number of memhandles in the list
 * @return			0 on success or negative on error
 *
 */
int nvhost_buffer_pin(struct nvhost_buffers *nvhost_buffers,
			struct dma_buf **dmabufs,
			u32 count);

/**
 * @brief			UnPins the mapped address space.
 *
 * @param nvhost_buffers	Pointer to nvhost_buffer struct
 * @param dmabufs		Pointer to dmabuffer list
 * @param count			Number of memhandles in the list
 * @return			None
 *
 */
void nvhost_buffer_unpin(struct nvhost_buffers *nvhost_buffers,
				struct dma_buf **dmabufs,
				u32 count);

/**
 * @brief			Pin the mapped buffer for a task submit
 *
 * This function increased the reference count for a mapped buffer during
 * task submission.
 *
 * @param nvhost_buffers	Pointer to nvhost_buffer struct
 * @param dmabufs		Pointer to dmabuffer list
 * @param count			Number of memhandles in the list
 * @param paddr			Pointer to IOVA list
 * @param psize			Pointer to size of buffer to return
 * @return			0 on success or negative on error
 *
 */
int nvhost_buffer_submit_pin(struct nvhost_buffers *nvhost_buffers,
				struct dma_buf **dmabufs, u32 count,
				dma_addr_t *paddr, size_t *psize);

/**
 * @brief		UnPins the mapped address space on task completion.
 *
 * This function decrease the reference count for a mapped buffer when the
 * task get completed or aborted.
 *
 * @param nvhost_buffers	Pointer to nvhost_buffer struct
 * @param dmabufs		Pointer to dmabuffer list
 * @param count			Number of memhandles in the list
 * @return			None
 *
 */
void nvhost_buffer_submit_unpin(struct nvhost_buffers *nvhost_buffers,
					struct dma_buf **dmabufs, u32 count);

/**
 * @brief			Cleanup all the buffers in the list
 *
 * @param nvhost_buffers	Pointer to nvhost_buffer struct
 * @return			None
 *
 */
void nvhost_buffer_put(struct nvhost_buffers *nvhost_buffers);

/**
 * @brief		Returns dma buf and dma addr for a given handle
 *
 * @param nvhost_buffers	Pointer to nvhost_buffer struct
 * @param dmabuf		dma buf pointer to search for
 * @param addr			dma_addr_t pointer to return
 * @return			0 on success or negative on error
 *
 */
int nvhost_get_iova_addr(struct nvhost_buffers *nvhost_buffers,
			struct dma_buf *dmabuf, dma_addr_t *addr);

#endif /*__NVHOST_NVHOST_BUFFER_H__ */
