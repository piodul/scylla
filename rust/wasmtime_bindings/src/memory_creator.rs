/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use anyhow::{anyhow, Result};
use std::{cmp, ptr, slice, u32};
use wasmtime::LinearMemory;

const WASM_PAGE_SIZE: usize = 64 * 1024;

extern "C" {
    fn aligned_alloc(align: usize, size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

pub struct ScyllaLinearMemory {
    ptr: *mut u8,
    size: usize,
    maximum_size: Option<usize>,
}

// The entire ScyllaLinearMemory is used only in a single thread,
// because we're not sharing it between seastar shards
unsafe impl Send for ScyllaLinearMemory {}
unsafe impl Sync for ScyllaLinearMemory {}

impl Drop for ScyllaLinearMemory {
    fn drop(&mut self) {
        // previously allocated or reset to nullptr in grow_to()
        unsafe { free(self.ptr) };
    }
}

unsafe impl LinearMemory for ScyllaLinearMemory {
    fn byte_size(&self) -> usize {
        self.size
    }
    fn maximum_byte_size(&self) -> Option<usize> {
        self.maximum_size
    }
    fn grow_to(&mut self, new_size: usize) -> Result<()> {
        let new_size_aligned = (new_size + WASM_PAGE_SIZE - 1) & !(WASM_PAGE_SIZE - 1);
        if new_size_aligned == self.size {
            return Ok(());
        }
        let max_size = self.maximum_size.unwrap_or(u32::MAX as usize);
        assert!(new_size_aligned <= max_size);
        let new_ptr: *mut u8;
        if new_size_aligned == 0 {
            new_ptr = ptr::null_mut()
        } else {
            new_ptr = unsafe { aligned_alloc(WASM_PAGE_SIZE, new_size_aligned) };
            if new_ptr.is_null() {
                return Err(anyhow!("Failed to grow WASM memory: allocation error"));
            }
        }
        let copy_size = cmp::min(self.size, new_size_aligned);
        unsafe {
            slice::from_raw_parts_mut(new_ptr, copy_size)
                .copy_from_slice(slice::from_raw_parts(self.ptr, copy_size))
        };
        unsafe { free(self.ptr) };
        self.size = new_size_aligned;
        self.ptr = new_ptr;
        Ok(())
    }
    fn as_ptr(&self) -> *mut u8 {
        self.ptr
    }
}

// In order to use the Seastar memory allocator instead of mmap,
// create our own MemoryCreator which directly calls aligned_alloc
// and free, both of which came from Seastar
pub struct ScyllaMemoryCreator;

unsafe impl wasmtime::MemoryCreator for ScyllaMemoryCreator {
    fn new_memory(
        &self,
        ty: wasmtime::MemoryType,
        minimum: usize,
        maximum: Option<usize>,
        reserved_size_in_bytes: Option<usize>,
        guard_size_in_bytes: usize,
    ) -> Result<Box<dyn wasmtime::LinearMemory>, String> {
        // assert that this is a memory that only allocates as much as it needs
        assert_eq!(guard_size_in_bytes, 0);
        assert!(reserved_size_in_bytes.is_none());
        assert!(!ty.is_64());
        let mut mem = ScyllaLinearMemory {
            ptr: ptr::null_mut(),
            size: 0,
            maximum_size: maximum,
        };
        if let Err(s) = mem.grow_to(minimum) {
            return Err(s.to_string());
        }
        Ok(Box::new(mem))
    }
}
