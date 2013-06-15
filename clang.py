#!/usr/bin/python
# -*- coding: utf-8 *-*

import os
import remote_pb2
import socket
import struct
import subprocess
import sys

def split_flags(argv):
  flags = dict()
  flags['compiler'] = list()
  flags['input'] = ''
  flags['linker'] = list()
  flags['mode'] = 'linker'
  flags['output'] = ''
  flags['preprocessor'] = list()

  eat_next = list()
  eat_until = ''
  for arg in argv[1:]:
    if eat_next:
      for eat in eat_next:
        if type(flags[eat]) is str:
          flags[eat] = arg
        elif type(flags[eat]) is list:
          flags[eat].append(arg)
      if not eat_until:
        eat_next = list()
      if arg == eat_until:
        eat_next = list()
        eat_until = ''
      continue

    # Check in alphabetical order
    if arg[0] == '-':
      if arg[1] == '-':
        if arg[2] == 'p':
          flags['compiler'].append(arg)

      elif arg[1] == 'a':
        if arg == '-arch':
          flags['compiler'].append(arg)
          flags['linker'].append(arg)
          flags['preprocessor'].append(arg)
          eat_next = ['compiler', 'linker', 'preprocessor']
      elif arg[1] == 'B':
        flags['linker'].append(arg)
      elif arg[1] == 'c':
        flags['mode'] = 'compiler'
      elif arg[1] == 'D':
        flags['preprocessor'].append(arg)
      elif arg[1] == 'f':
        flags['compiler'].append(arg)
        flags['preprocessor'].append(arg)
        if arg == '-fPIC':
          flags['linker'].append(arg)
      elif arg[1] == 'g':
        flags['compiler'].append(arg)
      elif arg[1] == 'I':
        flags['preprocessor'].append(arg)
      elif arg[1] == 'i':
        flags['preprocessor'].append(arg)
        eat_next = ['preprocessor']
      elif arg[1] == 'l':
        flags['linker'].append(arg)
      elif arg[1] == 'M':
        flags['preprocessor'].append(arg)
        if arg == '-MF':
          eat_next = ['preprocessor']
      elif arg[1] == 'm':
        flags['compiler'].append(arg)
        flags['preprocessor'].append(arg)
      elif arg[1] == 'O':
        flags['compiler'].append(arg)
      elif arg[1] == 'o':
        eat_next = ['output']
      elif arg[1] == 'p':
        flags['compiler'].append(arg)
        if arg == '-pthread':
          flags['linker'].append(arg)
      elif arg[1] == 's':
        flags['compiler'].append(arg)
      elif arg[1] == 'W':
        if len(arg) >= 3 and arg[2] == 'l' and arg[3] == ',':
          flags['linker'].append(arg)
          if arg == '-Wl,--start-group':
            eat_next = ['linker']
            eat_until = '-Wl,--end-group'
        else:
          flags['compiler'].append(arg)
      elif arg[1] == 'X':
        flags['compiler'].append(arg)
        eat_next = ['compiler']

    else:
      assert not flags['input']
      flags['input'] = arg

  assert not (eat_next or eat_until)

  return flags

def Main(argv):
  flags = split_flags(argv)

  argv[0] = os.environ['DIST_CLANG_CXX']

  if flags['mode'] == 'compiler':
    preprocessed_file = flags['output'] + os.path.splitext(flags['input'])[1]
    preprocess = flags['preprocessor'][:]

    # Append '-MT' flag in case there is '-MMD' flag.
    preprocess[0:0] = ['-MT', flags['output']]

    preprocess[0:0] = [argv[0], '-E', flags['input'], '-o', preprocessed_file]
    try:
      subprocess.check_call(preprocess)
    except:
      print 'Preprocessing failed!'
      print preprocess
      sys.exit(1)

    llvm_bc_file = os.path.splitext(flags['output'])[0] + '.bc'

    # LLVM doesn't support 'blocks' by default.
    flags['compiler'].append('-fblocks')

    fd = open(preprocessed_file)
    message = remote_pb2.Top()
    message.file_name = os.path.basename(flags['input'])
    message.command_line = " ".join(flags['compiler'])
    message.content = fd.read()
    fd.close()
    os.remove(preprocessed_file)

    request = socket.socket()
    request.connect((os.environ['DIST_CLANG_HOST'], os.environ['DIST_CLANG_PORT']))
    send_size = struct.pack('i', message.ByteSize())
    request.sendall(send_size)
    request.sendall(message.SerializeToString())

    size, = struct.unpack('i', request.recv(4))
    message_str = ''
    while len(message_str) < size:
      message_str = ''.join([message_str, request.recv(size - len(message_str))])

    message = remote_pb2.Top()
    message.ParseFromString(message_str)
    fd = open(llvm_bc_file, 'w+')
    fd.write(message.content)
    fd.close()
    request.close()

    object_file = flags['output']
    compile_to_native = flags['compiler'][:]
    compile_to_native[0:0] = [argv[0], '-c', llvm_bc_file, '-o', object_file]
    try:
      subprocess.check_call(compile_to_native)
    except:
      print 'Local compilation failed!'
      print compile_to_native
      sys.exit(1)
    finally:
      os.remove(llvm_bc_file)
  else:
    subprocess.check_call(argv)

# Run Main.
try:
  Main(sys.argv)
except KeyboardInterrupt:
  pass
