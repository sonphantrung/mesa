fs = import('fs')

nvk_classes = [
  'cl902d',
  'cl9039',
  'cl906f',
  'cl9097',
  'cl90b5',
  'cl90c0',
  'cla0b5',
  'cla097',
  'cla1c0',
  'clb097',
  'clb0c0',
  'clb197',
  'clb1c0',
  'clc097',
  'clc1b5',
  'cla0c0',
  'clc0c0',
  'clc397',
  'clc3c0',
  'clc597',
  'clc5c0',
  'clc6c0',
]

nvk_cl_header_depend_files = [
  files('class_parser.py')
]

cl_generated = []
foreach cl : nvk_classes
  headers = ['nvidia/classes/'+cl+'.h']
  if fs.exists('mixin/classes/'+cl+'.h')
    headers += ['mixin/classes/'+cl+'.h']
  endif
  command = [prog_python, '@INPUT0@',
             '--out_h', '@OUTPUT0@', '--out_c', '@OUTPUT1@']
  foreach i : range(headers.length())
    command += ['--in_h', '@INPUT'+(i+1).to_string()+'@']
  endforeach
  cl_generated += custom_target(
    cl + '.h',
    input : ['class_parser.py'] + headers,
    output : ['nvk_'+cl+'.h', 'nvk_'+cl+'.c'],
    command : command,
    depend_files: nvk_cl_header_depend_files,
  )
endforeach

_libnvidia_headers = static_library(
  'nvidia_headers',
  ['nv_push.c', cl_generated],
  include_directories : ['.', inc_include, inc_src],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
)

idep_nvidia_headers = declare_dependency(
  include_directories : include_directories('.'),
  sources : cl_generated,
  link_with : _libnvidia_headers,
)
