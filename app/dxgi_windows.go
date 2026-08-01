//go:build windows

package main

/*
#cgo windows LDFLAGS: -ldxgi
#include <stdlib.h>

int dxgi_adapter_count(void);
int dxgi_adapter_name(int index, char *buffer, int buffer_size);
*/
import "C"

import "unsafe"

type gpuAdapter struct {
	index int
	name  string
}

func dxgiGPUAdapters() []gpuAdapter {
	count := int(C.dxgi_adapter_count())
	if count <= 0 {
		return nil
	}

	adapters := make([]gpuAdapter, 0, count)
	buffer := make([]byte, 512)
	for index := 0; index < count; index++ {
		if C.dxgi_adapter_name(
			C.int(index),
			(*C.char)(unsafe.Pointer(&buffer[0])),
			C.int(len(buffer)),
		) == 0 {
			continue
		}
		name := string(buffer[:0])
		for i, value := range buffer {
			if value == 0 {
				name = string(buffer[:i])
				break
			}
		}
		if name != "" {
			adapters = append(adapters, gpuAdapter{index: index, name: name})
		}
	}
	return adapters
}
