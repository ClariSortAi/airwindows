// Package identity of this process (an MSIX install from the Microsoft Store, or a
// local registration of the same package). An unpackaged run has none.
#pragma once

#include <string>

namespace package_info {

// True when the process has package identity.
bool HasIdentity();

// The package family name ("Name_PublisherHash"); empty without package identity.
std::wstring FamilyName();

// The package's security identifier as a string ("S-1-15-2-..."), which Windows Firewall
// rules from the package manifest name; empty without package identity or on failure.
std::wstring Sid();

// The package's own local data folder (Windows.Storage.ApplicationData.Current.LocalFolder).
// Files written there are written as is: no AppData redirection applies. Empty without
// package identity or when the call fails; `error` then says why. Safe to call before
// COM is initialised: the call runs on a thread of its own.
std::wstring LocalFolder(std::wstring& error);

}  // namespace package_info
