
cmake_minimum_required(VERSION 3.0)

set(KF5_VERSION "5.53.0") # handled by release scripts
set(KF5_DEP_VERSION "5.52.0") # handled by release scripts
project(KService VERSION ${KF5_VERSION})

# Disallow in-source build
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
   message(FATAL_ERROR "KService requires an out of source build. Please create a separate build directory and run 'cmake path_to_kservice [options]' there.")
endif()

# ECM setup
include(FeatureSummary)
find_package(ECM 5.52.0  NO_MODULE)
set_package_properties(ECM PROPERTIES TYPE REQUIRED DESCRIPTION "Extra CMake Modules." URL "https://projects.kde.org/projects/kdesupport/extra-cmake-modules")
feature_summary(WHAT REQUIRED_PACKAGES_NOT_FOUND FATAL_ON_MISSING_REQUIRED_PACKAGES)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/cmake )

include(KDEInstallDirs)
include(KDEFrameworkCompilerSettings NO_POLICY_SCOPE)
include(KDECMakeSettings)
include(ECMSetupVersion)
include(ECMGenerateHeaders)
include(ECMAddQch)
include(ECMQtDeclareLoggingCategory)
include(GenerateExportHeader)

option(BUILD_QCH "Build API documentation in QCH format (for e.g. Qt Assistant, Qt Creator & KDevelop)" OFF)
add_feature_info(QCH ${BUILD_QCH} "API documentation in QCH format (for e.g. Qt Assistant, Qt Creator & KDevelop)")

ecm_setup_version(PROJECT
                  VARIABLE_PREFIX KSERVICE
                  VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kservice_version.h"
                  PACKAGE_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/KF5ServiceConfigVersion.cmake"
                  SOVERSION 5)

set(APPLICATIONS_MENU_NAME applications.menu
    CACHE STRING "Name to install the applications.menu file as.")

# Dependencies
set(REQUIRED_QT_VERSION 5.9.0)
find_package(Qt5 ${REQUIRED_QT_VERSION} CONFIG REQUIRED DBus Xml)

find_package(KF5Config ${KF5_DEP_VERSION} REQUIRED)
find_package(KF5CoreAddons ${KF5_DEP_VERSION} REQUIRED)
find_package(KF5Crash ${KF5_DEP_VERSION} REQUIRED)
find_package(KF5DBusAddons ${KF5_DEP_VERSION} REQUIRED)
find_package(KF5I18n ${KF5_DEP_VERSION} REQUIRED)
find_package(KF5DocTools ${KF5_DEP_VERSION})

find_package(FLEX REQUIRED)
set_package_properties(FLEX PROPERTIES
    URL "http://flex.sourceforge.net"
    DESCRIPTION "Fast Lexical Analyzer"
    TYPE REQUIRED
    PURPOSE "Required for the Trader parser"
)

find_package(BISON REQUIRED)
set_package_properties(BISON PROPERTIES
    URL "http://www.gnu.org/software/bison"
    DESCRIPTION "general-purpose parser generator"
    TYPE REQUIRED
    PURPOSE "Required for the Trader parser"
)

add_definitions(-DTRANSLATION_DOMAIN=\"kservice5\")

if (IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/po")
    ki18n_install(po)

    if (KF5DocTools_FOUND)
        kdoctools_install(po)
    endif()
endif()

if (KF5DocTools_FOUND)
    add_subdirectory(docs)
endif()

add_subdirectory(src)
if (BUILD_TESTING)
    add_subdirectory(autotests)
    add_subdirectory(tests)
endif()

# create a Config.cmake and a ConfigVersion.cmake file and install them
set(CMAKECONFIG_INSTALL_DIR "${KDE_INSTALL_CMAKEPACKAGEDIR}/KF5Service")

if (BUILD_QCH)
    ecm_install_qch_export(
        TARGETS KF5Service_QCH
        FILE KF5ServiceQchTargets.cmake
        DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
        COMPONENT Devel
    )
    set(PACKAGE_INCLUDE_QCHTARGETS "include(\"\${CMAKE_CURRENT_LIST_DIR}/KF5ServiceQchTargets.cmake\")")
endif()

include(CMakePackageConfigHelpers)

configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/KF5ServiceConfig.cmake.in"
                              "${CMAKE_CURRENT_BINARY_DIR}/KF5ServiceConfig.cmake"
                              INSTALL_DESTINATION  ${CMAKECONFIG_INSTALL_DIR}
                             )

install(EXPORT KF5ServiceTargets DESTINATION "${CMAKECONFIG_INSTALL_DIR}" FILE KF5ServiceTargets.cmake NAMESPACE KF5:: )

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/KF5ServiceConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/KF5ServiceConfigVersion.cmake"
              "${CMAKE_CURRENT_SOURCE_DIR}/KF5ServiceMacros.cmake"
        DESTINATION "${CMAKECONFIG_INSTALL_DIR}"
        COMPONENT Devel
       )

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kservice_version.h
        DESTINATION ${KDE_INSTALL_INCLUDEDIR_KF5} COMPONENT Devel )

# contains list of debug categories, for kdebugsettings
install(FILES kservice.categories DESTINATION ${KDE_INSTALL_CONFDIR})

feature_summary(WHAT ALL   FATAL_ON_MISSING_REQUIRED_PACKAGES)
