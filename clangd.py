#!/usr/bin/python
# -*- coding: utf-8 -*-

import argparse
import os
import remote_pb2
import SocketServer
import struct
import subprocess
import sys
import tempfile

class Handler(SocketServer.BaseRequestHandler):
  def handle(self):
    size, = struct.unpack('i', self.request.recv(4))
    message_str = ''
    while len(message_str) < size:
      message_str = ''.join([message_str, self.request.recv(size - len(message_str))])

    message = remote_pb2.Top()
    message.ParseFromString(message_str)
    tmp_dir = tempfile.mkdtemp()
    preprocessed_file = os.path.join(tmp_dir, message.file_name)
    fd = open(preprocessed_file, 'w+')
    fd.write(message.content)
    fd.close()

    compiler = os.environ['DIST_CLANG_CXX']
    llvm_bc_file = os.path.splitext(preprocessed_file)[0] + '.bc'
    args = message.command_line.split()

    # Filter Mac-specific flags
    args = filter(lambda a: a != '-mmacosx-version-min=10.6', args[:])

    # Filter plugin path
    args = [x if not x.count('libFindBadConstructs') else os.environ['CLANG_PLUGIN'] for x in args[:]]

    # Filter 'arch' flag
    if args.count('-arch'):
      arch_index = args.index('-arch')
      if args[arch_index+1] == 'i386':
        args[arch_index+1] = '-m32'
      else:
        del args[arch_index+1]
      del args[arch_index]

    args[0:0] = [compiler, '-c', '-emit-llvm', preprocessed_file, '-o', llvm_bc_file]
    subprocess.call(args)
    os.remove(preprocessed_file)

    fd = open(llvm_bc_file)
    send_message = remote_pb2.Top()
    send_message.file_name = llvm_bc_file
    send_message.content = fd.read()
    fd.close()
    send_size = struct.pack('i', send_message.ByteSize())
    self.request.sendall(send_size)
    self.request.sendall(send_message.SerializeToString())
    os.remove(llvm_bc_file)
    os.rmdir(tmp_dir)

def Main(argv):
  parser = argparse.ArgumentParser(description = 'Daemon for clang distributed compilation.')
  parser.add_argument('-l', '--listen', default='0.0.0.0:60000', help="Listen on interface, i.e. 0.0.0.0:60000")

  args = parser.parse_args()
  host, port = args.listen.split(':')
  server = SocketServer.TCPServer((host, int(port)), Handler)
  server.allow_reuse_address = True
  server.serve_forever()

# Run Main.
try:
  Main(sys.argv)
except KeyboardInterrupt:
  pass
