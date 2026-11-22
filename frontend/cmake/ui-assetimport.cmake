# Siegfried: Scene Collection > Import Assets (overlay bundles)
find_package(ZLIB REQUIRED)

target_sources(
  obs-studio
  PRIVATE
    assetimport/AssetPackage.hpp
    assetimport/ImportAssetsDialog.cpp
    assetimport/ImportAssetsDialog.hpp
    assetimport/PackageInstaller.cpp
    assetimport/PackageScanner.cpp
    assetimport/ZipReader.cpp
    assetimport/ZipReader.hpp
)

target_link_libraries(obs-studio PRIVATE ZLIB::ZLIB)
