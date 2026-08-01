package main

import "runtime"

func isWindows() bool { return runtime.GOOS == "windows" }
func isDarwin() bool  { return runtime.GOOS == "darwin" }
