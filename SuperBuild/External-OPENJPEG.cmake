set(OPENJPEG_TAG  v2.5.3)

ExternalProject_Add(openjpeg
    GIT_REPOSITORY "https://github.com/uclouvain/openjpeg.git"
    GIT_TAG "${OPENJPEG_TAG}"
    SOURCE_DIR openjpeg
    BINARY_DIR openjpeg-build
    CMAKE_ARGS
        -Wno-dev
        --no-warn-unused-cli
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
        -DBUILD_SHARED_LIBS=OFF
        # disable optional dependencies not needed for dcm2niix
        -DBUILD_CODEC=OFF
        -DBUILD_JPIP=OFF
        -DBUILD_JPWL=OFF
        -DBUILD_VIEWER=OFF
        -DBUILD_JAVA=OFF
        -DBUILD_MJ2=OFF
        -DBUILD_THIRDPARTY=OFF
        -DCMAKE_INSTALL_PREFIX:PATH=${DEP_INSTALL_DIR}
)

include(GNUInstallDirs)
set(OpenJPEG_DIR ${DEP_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/cmake/openjpeg-2.5)
