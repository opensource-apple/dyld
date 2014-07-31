# stuff to include in every test Makefile

SHELL = /bin/sh

CC		 = gcc-4.0 ${ARCH}
CCFLAGS = -Wall -g -std=c99

CXX		  = g++-4.0 ${ARCH}
CXXFLAGS = -Wall -g

RM      = rm
RMFLAGS = -rf
