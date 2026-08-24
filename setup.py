from setuptools import Extension, setup
import numpy

setup(name='epf-cpy', ext_modules=[Extension('cpy', ['cpy.pyx'], include_dirs=[numpy.get_include()])])
