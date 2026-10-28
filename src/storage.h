#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "database.h"

namespace storage {

// First path segment of a virtual path (the root name), or the whole string
// when there is no '/'.
std::string rootNameOf(const std::string &virtual_path);

// Look up a scan root by its name.
std::optional<repo::ScanRoot> findRootByName(const std::string &name);

// Convert a virtual path (<name>/<relative>) into a real path / URL using the
// given root.  If the virtual path does not belong to this root it is returned
// unchanged.
std::string toReal(const repo::ScanRoot &root, const std::string &virtual_path);

// Convert a real path / URL into a virtual path using the given root.  If the
// real path is not under the root it is returned unchanged.
std::string toVirtual(const repo::ScanRoot &root, const std::string &real_path);

// Resolve a virtual path by looking up its root name.  Falls back to returning
// the input unchanged when no matching root exists.
std::string resolve(const std::string &virtual_path);

// Return true when target (after weakly_canonical) is inside one of the
// configured scan roots.
bool isUnderAnyRoot(const std::filesystem::path &target);

} // namespace storage
