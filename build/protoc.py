#!/usr/bin/env python3
# Usage: protoc.py <protoc> <proto_path> <cpp_out> <source.proto>

import subprocess
import sys

protoc, proto_path, cpp_out, source = sys.argv[1:]
sys.exit(subprocess.call([protoc, "--proto_path=" + proto_path,
                          "--cpp_out=" + cpp_out, source]))
