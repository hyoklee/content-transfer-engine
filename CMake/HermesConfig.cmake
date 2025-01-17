# Find Hermes header and library.
#

# This module defines the following uncached variables:
#  Hermes_FOUND, if false, do not try to use Hermes.
#  Hermes_INCLUDE_DIRS, where to find Hermes.h.
#  Hermes_LIBRARIES, the libraries to link against to use the Hermes library
#  Hermes_LIBRARY_DIRS, the directory where the Hermes library is found.

#-----------------------------------------------------------------------------
# Define constants
#-----------------------------------------------------------------------------
set(HERMES_VERSION_MAJOR @HERMES_VERSION_MAJOR@)
set(HERMES_VERSION_MINOR @HERMES_VERSION_MINOR@)
set(HERMES_VERSION_PATCH @HERMES_VERSION_PATCH@)

set(BUILD_MPI_TESTS @BUILD_MPI_TESTS@)
set(BUILD_OpenMP_TESTS @BUILD_OpenMP_TESTS@)
set(HERMES_ENABLE_COVERAGE @HERMES_ENABLE_COVERAGE@)
set(HERMES_ENABLE_DOXYGEN @HERMES_ENABLE_DOXYGEN@)
set(HERMES_REMOTE_DEBUG @HERMES_REMOTE_DEBUG@)

set(HERMES_ENABLE_POSIX_ADAPTER @HERMES_ENABLE_POSIX_ADAPTER@)
set(HERMES_ENABLE_STDIO_ADAPTER @HERMES_ENABLE_STDIO_ADAPTER@)
set(HERMES_ENABLE_MPIIO_ADAPTER @HERMES_ENABLE_MPIIO_ADAPTER@)
set(HERMES_ENABLE_VFD @HERMES_ENABLE_VFD@)
set(HERMES_ENABLE_PUBSUB_ADAPTER @HERMES_ENABLE_PUBSUB_ADAPTER@)
set(HERMES_ENABLE_KVSTORE @HERMES_ENABLE_KVSTORE@)
set(HERMES_ENABLE_PYTHON @HERMES_ENABLE_PYTHON@)
set(HERMES_ENABLE_ADIOS @HERMES_ENABLE_ADIOS@)

set(HERMES_MPICH @HERMES_MPICH@)
set(HERMES_OPENMPI @HERMES_OPENMPI@)

# Find the Hermes Package
find_package(HermesCore REQUIRED)

# Find the Hermes dependencies
find_package(HermesCommon REQUIRED)
