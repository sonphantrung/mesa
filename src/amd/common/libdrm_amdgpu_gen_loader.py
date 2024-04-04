# Copyright 2023 Advanced Micro Devices, Inc.
# All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# on the rights to use, copy, modify, merge, publish, distribute, sub
# license, and/or sell copies of the Software, and to permit persons to whom
# the Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
# THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
# USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Generate dlopen/dlsym code for libdrm_amdgpu symbols we need.
import sys
from pycparser import c_ast, parse_file, c_generator

# FIXME

generator = c_generator.CGenerator()
# A simple visitor for FuncDef nodes that prints the names and
# locations of function definitions.
class FuncDefVisitor(c_ast.NodeVisitor):
    def __init__(self, header):
        self.types = []
        self.header = header


    def visit_FuncDecl(self, node):
        return_type = generator._generate_type(node.type.type, emit_declname=False)

        if isinstance(node.type, c_ast.PtrDecl):
            ctype = '{}_type'.format(node.type.type.declname)
            if self.header:
                print('typedef {}* (*{})'.format(return_type, ctype), end='')
        else:
            ctype = '{}_type'.format(node.type.declname)
            if self.header:
                print('typedef {} (*{})'.format(return_type, ctype), end='')

        self.types += [ctype]
        args = []
        for p in node.args.params:
            if p.name is None:
                continue
            args += [generator._generate_decl(p)]
        if self.header:
            print('({});'.format(', '.join(args)))

class FuncStubVisitor(c_ast.NodeVisitor):
    def __init__(self):
        self.types = []

    def visit_FuncDecl(self, node):
        return_type = generator._generate_type(node.type.type, emit_declname=False)

        if isinstance(node.type, c_ast.PtrDecl):
            print('static {}* {}_stub'.format(return_type, node.type.type.declname), end='')
        else:
            print('static {} {}_stub'.format(return_type, node.type.declname), end='')

        args = []
        for p in node.args.params:
            if p.name is None:
                continue
            args += [generator._generate_decl(p)]
        # args = [generator._generate_decl(p) for p in node.args.params]

        print('({})'.format(', '.join(args)), end='')

        print('''
            {
            printf("IMPLEMENT ME %s\\n", __FUNCTION__);
            assert(!getenv("VIRTIO_MISSING"));
        ''', end='')

        s = '{}'.format(return_type)
        if s == 'int ' or s == 'uint64_t ':
            print(' return -1; }')
        elif s == 'void ':
            print('}')
        else:
            print('return NULL; }')

def generate_header(filename):
    # Note that cpp is used. Provide a path to your own cpp or
    # make sure one exists in PATH.

    ast = parse_file(filename, use_cpp=True)
    print('''
#ifndef AC_LIBDRM_H
#define AC_LIBDRM_H
#include <amdgpu.h>
#include <stdint.h>
        ''')

    # Generate typedefs
    v = FuncDefVisitor(True)
    v.visit(ast)

    # Generate struct
    print('struct libdrm_amdgpu {')
    print('   void *handle;')
    print('   int32_t refcount;')
    for ty in v.types:
        print('{} {};'.format(ty, ty[len('amdgpu_'):-len('_type')]))
    print('};')

    print('''
#ifdef __cplusplus
extern "C" {
#endif

struct libdrm_amdgpu * ac_init_libdrm_amdgpu(void);

struct libdrm_amdgpu * ac_init_libdrm_amdgpu_for_virtio(void);

struct libdrm_amdgpu * ac_init_libdrm_amdgpu_for_virtio_stubs(void);

#ifdef __cplusplus
}
#endif

#endif
''')

def generate_source(filename):
    # Note that cpp is used. Provide a path to your own cpp or
    # make sure one exists in PATH.

    ast = parse_file(filename, use_cpp=True)
    print('''
#include "libdrm_amdgpu_loader.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

struct libdrm_amdgpu * ac_init_libdrm_amdgpu(void) {
   struct libdrm_amdgpu *libdrm_amdgpu = calloc(1, sizeof(struct libdrm_amdgpu));
   void *libdrm = dlopen("libdrm_amdgpu.so.1", RTLD_NOW | RTLD_LOCAL);
   assert(libdrm);
        ''')

    # Generate typedefs
    v = FuncDefVisitor(False)
    v.visit(ast)

    # Generate struct
    for ty in v.types:
        local = ty[len('amdgpu_'):-len('_type')]
        original = ty[:-len('_type')]
        print('libdrm_amdgpu->{} = dlsym(libdrm, "{}");'.format(local, original))
        print('assert(libdrm_amdgpu->{} != NULL);'.format(local))

    print('libdrm_amdgpu->handle = libdrm;')
    print('''
   return libdrm_amdgpu;
}
''')
    FuncStubVisitor().visit(ast)

    print('''
    struct libdrm_amdgpu * ac_init_libdrm_amdgpu_for_virtio_stubs(void) {
       struct libdrm_amdgpu *libdrm_amdgpu = calloc(1, sizeof(struct libdrm_amdgpu));
''')
    for ty in v.types:
        local = ty[len('amdgpu_'):-len('_type')]
        stub = '{}_stub'.format(ty[:-len('_type')])
        print('libdrm_amdgpu->{} = {};'.format(local, stub))

    print('''
   return libdrm_amdgpu;
}
''')

if __name__ == "__main__":
    # TODO: generate default stubs function for virtio. This way when we
    # hit an unimplemented function will get a log like "TODO: implement amdgpu..."
    # instead of a crash when the app tries to call a NULL pointer.
    if len(sys.argv) != 3:
        print('Usage: {} path/to/amdgpu.h header|source'.format(sys.argv[0]))
        sys.exit(-1)

    if sys.argv[2] == 'header':
        generate_header(sys.argv[1])
    elif sys.argv[2] == 'source':
        generate_source(sys.argv[1])
    else:
        print(sys.argv)
        sys.exit(-1)