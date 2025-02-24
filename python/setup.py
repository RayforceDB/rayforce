from setuptools import setup, Extension
import os
import sys

rayforce_module = Extension(
    'rayforce._rayforce',
    sources=['./rayforce_python.c'],
    include_dirs=['../core'],
    library_dirs=['../lib'],
    libraries=['rayforce'],
    extra_compile_args=['-Wall', '-O3', '-fPIC', '-Wno-implicit-function-declaration', '-arch', 'arm64', '-Wno-visibility', '-fvisibility=default'],
    extra_link_args=['-Wl,-rpath,../lib', '-arch', 'arm64'],
    define_macros=[('PY_SSIZE_T_CLEAN', None), ('SWIG_PYTHON3', None)],
)

setup(
    name='rayforce',
    version='0.1',
    description='Python bindings for RayforceDB',
    ext_modules=[rayforce_module],
    author='Anton Kundenko',
    author_email='singaraiona@gmail.com',
    url='https://github.com/singaraiona/rayforce',
    python_requires='>=3.9',
    packages=['rayforce'],
    package_data={'rayforce': ['../lib/librayforce.so']},
) 