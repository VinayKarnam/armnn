//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "DynamicBackendUtils.hpp"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <regex>

namespace armnn
{

void* DynamicBackendUtils::OpenHandle(const std::string& sharedObjectPath)
{
    if (sharedObjectPath.empty())
    {
        throw RuntimeException("OpenHandle error: shared object path must not be empty");
    }

    void* sharedObjectHandle = dlopen(sharedObjectPath.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!sharedObjectHandle)
    {
        throw RuntimeException(boost::str(boost::format("OpenHandle error: %1%") % GetDlError()));
    }

    return sharedObjectHandle;
}

void DynamicBackendUtils::CloseHandle(const void* sharedObjectHandle)
{
    if (!sharedObjectHandle)
    {
        return;
    }

    dlclose(const_cast<void*>(sharedObjectHandle));
}

bool DynamicBackendUtils::IsBackendCompatible(const BackendVersion &backendVersion)
{
    BackendVersion backendApiVersion = IBackendInternal::GetApiVersion();

    return IsBackendCompatibleImpl(backendApiVersion, backendVersion);
}

bool DynamicBackendUtils::IsBackendCompatibleImpl(const BackendVersion &backendApiVersion,
                                                  const BackendVersion &backendVersion)
{
    return backendVersion.m_Major == backendApiVersion.m_Major &&
           backendVersion.m_Minor <= backendApiVersion.m_Minor;
}

std::string DynamicBackendUtils::GetDlError()
{
    const char* errorMessage = dlerror();
    if (!errorMessage)
    {
        return "";
    }

    return std::string(errorMessage);
}

std::vector<std::string> DynamicBackendUtils::GetBackendPaths(const std::string& overrideBackendPath)
{
    // Check if a path where to dynamically load the backends from is given
    if (!overrideBackendPath.empty())
    {
        if (!IsPathValid(overrideBackendPath))
        {
            BOOST_LOG_TRIVIAL(warning) << "WARNING: The given override path for dynamic backends \""
                                       << overrideBackendPath << "\" is not valid";

            return {};
        }

        return std::vector<std::string>{ overrideBackendPath };
    }

    // Expects a colon-separated list: DYNAMIC_BACKEND_PATHS="PATH_1:PATH_2:...:PATH_N"
    const std::string backendPaths = DYNAMIC_BACKEND_PATHS;

    return GetBackendPathsImpl(backendPaths);
}

std::vector<std::string> DynamicBackendUtils::GetBackendPathsImpl(const std::string& backendPaths)
{
    std::unordered_set<std::string> uniqueBackendPaths;
    std::vector<std::string> tempBackendPaths;
    std::vector<std::string> validBackendPaths;

    // Split the given list of paths
    boost::split(tempBackendPaths, backendPaths, boost::is_any_of(":"));

    for (const std::string& path : tempBackendPaths)
    {
        // Check whether the path is valid
        if (!IsPathValid(path))
        {
            continue;
        }

        // Check whether the path is a duplicate
        auto it = uniqueBackendPaths.find(path);
        if (it != uniqueBackendPaths.end())
        {
            // The path is a duplicate
            continue;
        }

        // Add the path to the set of unique paths
        uniqueBackendPaths.insert(path);

        // Add the path to the list of valid paths
        validBackendPaths.push_back(path);
    }

    return validBackendPaths;
}

bool DynamicBackendUtils::IsPathValid(const std::string& path)
{
    if (path.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "WARNING: The given backend path is empty";
        return false;
    }

    boost::filesystem::path boostPath(path);

    if (!boost::filesystem::exists(boostPath))
    {
        BOOST_LOG_TRIVIAL(warning) << "WARNING: The given backend path \"" << path << "\" does not exist";
        return false;
    }

    if (!boost::filesystem::is_directory(boostPath))
    {
        BOOST_LOG_TRIVIAL(warning) << "WARNING: The given backend path \"" << path << "\" is not a directory";
        return false;
    }

    if (!boostPath.is_absolute())
    {
        BOOST_LOG_TRIVIAL(warning) << "WARNING: The given backend path \"" << path << "\" is not absolute";
        return false;
    }

    return true;
}

std::vector<std::string> DynamicBackendUtils::GetSharedObjects(const std::vector<std::string>& backendPaths)
{
    std::unordered_set<std::string> uniqueSharedObjects;
    std::vector<std::string> sharedObjects;

    for (const std::string& backendPath : backendPaths)
    {
        using namespace boost::filesystem;

        // Check if the path is valid. In case of error, IsValidPath will log an error message
        if (!IsPathValid(backendPath))
        {
            continue;
        }

        // Go through all the files in the current backend path
        for (directory_iterator fileIterator(backendPath); fileIterator != directory_iterator(); fileIterator++)
        {
            path filePath = *fileIterator;
            std::string filename = filePath.filename().string();

            if (filename.empty())
            {
                // Empty filename
                continue;
            }

            path canonicalPath;
            try
            {
                // Get the canonical path for the current file, it will throw if for example the file is a
                // symlink that cannot be resolved
                canonicalPath = canonical(filePath);
            }
            catch (const filesystem_error& e)
            {
                BOOST_LOG_TRIVIAL(warning) << "GetSharedObjects warning: " << e.what();
            }
            if (canonicalPath.empty())
            {
                // No such file or perhaps a symlink that couldn't be resolved
                continue;
            }

            // Check if the current filename matches the expected naming convention
            // The expected format is: <vendor>_<name>_backend.so[<version>]
            // e.g. "Arm_GpuAcc_backend.so" or "Arm_GpuAcc_backend.so.1.2"
            const std::regex dynamicBackendRegex("^[a-zA-Z0-9]+_[a-zA-Z0-9]+_backend.so(\\.[0-9]+)*$");

            bool filenameMatch = false;
            try
            {
                // Match the filename to the expected naming scheme
                filenameMatch = std::regex_match(filename, dynamicBackendRegex);
            }
            catch (const std::exception& e)
            {
                BOOST_LOG_TRIVIAL(warning) << "GetSharedObjects warning: " << e.what();
            }
            if (!filenameMatch)
            {
                // Filename does not match the expected naming scheme (or an error has occurred)
                continue;
            }

            // Append the valid canonical path to the output list only if it's not a duplicate
            std::string validCanonicalPath = canonicalPath.string();
            auto it = uniqueSharedObjects.find(validCanonicalPath);
            if (it == uniqueSharedObjects.end())
            {
                // Not a duplicate, append the canonical path to the output list
                sharedObjects.push_back(validCanonicalPath);

                // Add the canonical path to the collection of unique shared objects
                uniqueSharedObjects.insert(validCanonicalPath);
            }
        }
    }

    return sharedObjects;
}

} // namespace armnn