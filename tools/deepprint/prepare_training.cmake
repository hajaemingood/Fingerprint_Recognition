# Run explicitly from the repository root to install the two native CPU libraries.
cmake_minimum_required(VERSION 3.18)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(destination "${root}/build/dependencies/torch-native")
file(MAKE_DIRECTORY "${destination}")
function(install_native package version sha256)
    set(archive "${destination}/${package}.${version}.nupkg")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/${package}/${version}/${package}.${version}.nupkg"
        "${archive}" EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${destination}" PATTERNS "runtimes/win-x64/native/*")
endfunction()
install_native(torchsharp 0.105.0 6088be67231914bce651783921261397b7f9dbc5dd01115459a34899b37b44f9)
install_native(libtorch-cpu-win-x64 2.5.1 f207f8229e7bc57bf1c2fa59ad163d8b7fa38ebf319139e789eab37697c3e81f)
message(STATUS "Native training libraries: ${destination}/runtimes/win-x64/native")

