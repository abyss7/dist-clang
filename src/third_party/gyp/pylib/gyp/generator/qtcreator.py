# Copyright (c) 2013 Yandex LLC. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import filecmp
import fnmatch
import gyp
import gyp.common
import os.path
import shutil


generator_default_variables = {
  'CONFIGURATION_NAME': '$(CONFIGURATION)',
  'PRODUCT_DIR': '$(BUILT_PRODUCTS_DIR)',
  # unused vars
  'EXECUTABLE_PREFIX': '',
  'EXECUTABLE_SUFFIX': '',
  'STATIC_LIB_PREFIX': '',
  'STATIC_LIB_SUFFIX': '',
  'SHARED_LIB_PREFIX': '',
  'SHARED_LIB_SUFFIX': '',
  'INTERMEDIATE_DIR': 'dir',
  'RULE_INPUT_ROOT': '',
  'RULE_INPUT_PATH': '',
  'RULE_INPUT_EXT': '',
  'RULE_INPUT_DIRNAME': '',
  'SHARED_INTERMEDIATE_DIR': 'dir',
  'SHARED_LIB_DIR': 'dir',
  'LIB_DIR': 'dir',
}


def CalculateVariables(default_variables, params):
  default_variables.setdefault('OS', gyp.common.GetFlavor(params))


def GenerateOutput(target_list, target_dicts, data, params):
  options = params['options']
  generator_flags = params.get('generator_flags', {})
  toplevel_build = os.path.join(options.toplevel_dir, generator_flags.get('output_dir', 'out'))

  project_name = os.path.basename(os.path.normpath(os.path.join(toplevel_build, "..", "..")))

  def create_file(file_path):
    if not os.path.exists(file_path):
      open(file_path, 'w').close()

  create_file(os.path.join(toplevel_build, project_name + ".config"))
  create_file(os.path.join(toplevel_build, project_name + ".includes"))
  create_file(os.path.join(toplevel_build, project_name + ".creator"))
  create_file(os.path.join(toplevel_build, project_name + ".files"))

  # Generate .files
  new_dot_files_name = os.path.join(toplevel_build, project_name + ".files.new")
  old_dot_files_name = os.path.join(toplevel_build, project_name + ".files")
  files = []
  for root, dirnames, filenames in os.walk(options.toplevel_dir):
    for filename in fnmatch.filter(filenames, '*.gyp*'):
      files.append(os.path.join(root, filename))
  for target in target_dicts.iterkeys():
    dirname = os.path.dirname(target.split(':')[0])
    if 'sources' in target_dicts[target]:
      for source in target_dicts[target]['sources']:
        files.append(os.path.abspath(os.path.join(dirname, source)))
  files.sort()
  with open(new_dot_files_name, 'w') as project_files:
    for line in files:
      project_files.write(line + '\n')
  if filecmp.cmp(new_dot_files_name, old_dot_files_name):
    os.remove(new_dot_files_name)
  else:
    shutil.move(new_dot_files_name, old_dot_files_name)

  # TODO(ilezhankin): implement generation of the project configuration.
