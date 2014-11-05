#! /bin/bash

################################################################################
# Prepare
################################################################################

# Set up shell
if [ "$(echo ${VERBOSE} | tr '[:upper:]' '[:lower:]')" = 'yes' ]; then
    set -x                      # Output commands
fi
set -e                          # Abort on errors



################################################################################
# Search
################################################################################

if [ -z "${PAPI_DIR}" ]; then
    echo "BEGIN MESSAGE"
    echo "PAPI selected, but PAPI_DIR not set. Checking some places..."
    echo "END MESSAGE"
    
    DIRS="/usr /usr/local /opt/local ${HOME}"
    # look into each directory
    for dir in $DIRS; do
        # libraries might have different file extensions
        for libext in a so dylib; do
            # libraries can be in /lib or /lib64
            for libdir in lib64 lib; do
                # These files must exist
                FILES="include/papi.h ${libdir}/libpapi.${libext}"
                # assume this is the one and check all needed files
                PAPI_DIR="$dir"
                for file in $FILES; do
                    # discard this directory if one file was not found
                    if [ ! -r "$dir/$file" ]; then
                        unset PAPI_DIR
                        break
                    fi
                done
                # don't look further if all files have been found
                if [ -n "$PAPI_DIR" ]; then
                    break
                fi
            done
            # don't look further if all files have been found
            if [ -n "$PAPI_DIR" ]; then
                break
            fi
        done
        # don't look further if all files have been found
        if [ -n "$PAPI_DIR" ]; then
            break
        fi
    done
    
    if [ -z "$PAPI_DIR" ]; then
        echo "BEGIN MESSAGE"
        echo "PAPI not found"
        echo "END MESSAGE"
    else
        echo "BEGIN MESSAGE"
        echo "Found PAPI in ${PAPI_DIR}"
        echo "END MESSAGE"
    fi
fi



################################################################################
# Build
################################################################################

if [ -z "${PAPI_DIR}"                                            \
     -o "$(echo "${PAPI_DIR}" | tr '[a-z]' '[A-Z]')" = 'BUILD' ]
then
    echo "BEGIN MESSAGE"
    echo "Using bundled PAPI..."
    echo "END MESSAGE"
    
    # Set locations
    THORN=PAPI
    NAME=papi-5.3.0
    TARNAME=papi-5.3.0
    SRCDIR="$(dirname $0)"
    BUILD_DIR=${SCRATCH_BUILD}/build/${THORN}
    if [ -z "${PAPI_INSTALL_DIR}" ]; then
        INSTALL_DIR=${SCRATCH_BUILD}/external/${THORN}
    else
        echo "BEGIN MESSAGE"
        echo "Installing PAPI into ${PAPI_INSTALL_DIR} "
        echo "END MESSAGE"
        INSTALL_DIR=${PAPI_INSTALL_DIR}
    fi
    DONE_FILE=${SCRATCH_BUILD}/done/${THORN}
    PAPI_DIR=${INSTALL_DIR}
    
    if [ -e ${DONE_FILE} -a ${DONE_FILE} -nt ${SRCDIR}/dist/${TARNAME}.tar.gz \
                         -a ${DONE_FILE} -nt ${SRCDIR}/configure.sh ]
    then
        echo "BEGIN MESSAGE"
        echo "PAPI has already been built; doing nothing"
        echo "END MESSAGE"
    else
        echo "BEGIN MESSAGE"
        echo "Building PAPI"
        echo "END MESSAGE"
        
        # Build in a subshell
        (
        exec >&2                    # Redirect stdout to stderr
        if [ "$(echo ${VERBOSE} | tr '[:upper:]' '[:lower:]')" = 'yes' ]; then
            set -x                  # Output commands
        fi
        set -e                      # Abort on errors
        cd ${SCRATCH_BUILD}
        
        # Set up environment
        unset CPP
        unset LIBS
        export MPICC=:          # disable MPI tests
        if echo '' ${ARFLAGS} | grep 64 > /dev/null 2>&1; then
            export OBJECT_MODE=64
        fi
        
        echo "PAPI: Preparing directory structure..."
        mkdir build external done 2> /dev/null || true
        rm -rf ${BUILD_DIR} ${INSTALL_DIR}
        mkdir ${BUILD_DIR} ${INSTALL_DIR}
        
        echo "PAPI: Unpacking archive..."
        pushd ${BUILD_DIR}
        ${TAR?} xzf ${SRCDIR}/dist/${TARNAME}.tar.gz
        
        echo "PAPI: Applying patches..."
        pushd ${NAME}
        # Replace <malloc.h> by <stdlib.h>
        find . -type f -print | xargs perl -pi -e 's/malloc.h/stdlib.h/'
        # disable Werror since new compiler warns about PAPI
        find . -name config.mk -print | xargs perl -pi -e 's/-Werror//g'
        popd
        
        echo "PAPI: Configuring..."
        cd ${NAME}/src
        # CC          C compiler command
        # CFLAGS      C compiler flags
        # LDFLAGS     linker flags, e.g. -L<lib dir> if you have libraries in a
        #             nonstandard directory <lib dir>
        # CPPFLAGS    C/C++ preprocessor flags, e.g. -I<include dir> if you have
        #             headers in a nonstandard directory <include dir>
        # F77         Fortran 77 compiler command
        # FFLAGS      Fortran 77 compiler flags
        # CPP         C preprocessor
        #unset CC
        #unset CFLAGS
        #unset LDFLAGS
        #unset CPPFLAGS
        #unset F77
        #unset FFLAGS
        #unset CPP
        #export MPICC=

        ./configure --prefix=${PAPI_DIR} --with-shared-lib=no
        
        echo "PAPI: Building..."
        ${MAKE}
        
        echo "PAPI: Installing..."
        ${MAKE} install
        popd
        
        echo "PAPI: Cleaning up..."
        rm -rf ${BUILD_DIR}
        
        date > ${DONE_FILE}
        echo "PAPI: Done."
        )
        if (( $? )); then
            echo 'BEGIN ERROR'
            echo 'Error while building PAPI. Aborting.'
            echo 'END ERROR'
            exit 1
        fi
    fi
    
fi



################################################################################
# Configure Cactus
################################################################################

# Set options
PAPI_INC_DIRS="${PAPI_DIR}/include"
PAPI_LIB_DIRS="${PAPI_DIR}/lib"
PAPI_LIBS="papi"

if nm ${PAPI_LIB_DIRS}/libpapi.a | grep -q pm_initialize; then
    PAPI_LIBS="${PAPI_LIBS} pmapi"
fi

PAPI_INC_DIRS="$(${CCTK_HOME}/lib/sbin/strip-incdirs.sh ${PAPI_INC_DIRS})"
PAPI_LIB_DIRS="$(${CCTK_HOME}/lib/sbin/strip-libdirs.sh ${PAPI_LIB_DIRS})"

# Pass options to Cactus
echo "BEGIN MAKE_DEFINITION"
echo "PAPI_DIR      = ${PAPI_DIR}"
echo "PAPI_INC_DIRS = ${PAPI_INC_DIRS}"
echo "PAPI_LIB_DIRS = ${PAPI_LIB_DIRS}"
echo "PAPI_LIBS     = ${PAPI_LIBS}"
echo "END MAKE_DEFINITION"

echo 'INCLUDE_DIRECTORY $(PAPI_INC_DIRS)'
echo 'LIBRARY_DIRECTORY $(PAPI_LIB_DIRS)'
echo 'LIBRARY           $(PAPI_LIBS)'
