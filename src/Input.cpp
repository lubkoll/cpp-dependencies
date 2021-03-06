/*
 * Copyright (C) 2012-2016. TomTom International BV (http://tomtom.com).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Component.h"
#include "Configuration.h"
#include "FstreamInclude.h"
#include "Input.h"
#include <algorithm>

#ifdef WITH_MMAP
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef NO_MEMRCHR
const void* memrchr(const void* buffer, unsigned char value, size_t buffersize) {
  const unsigned char* buf = (const unsigned char*)buffer;
  do {
    buffersize--;
    if (buf[buffersize] == value) return buf + buffersize;
  } while(buffersize > 0);
  return NULL;
}
#endif

static void ReadCodeFrom(File& f, const char* buffer, size_t buffersize, bool withLoc) {
    if (buffersize == 0) return;
    size_t offset = 0;
    enum State { None, AfterHash, AfterInclude, InsidePointyIncludeBrackets, InsideStraightIncludeBrackets } state = None;
    // Try to terminate reading the file when we've read the last possible preprocessor command
    const char* lastHash = static_cast<const char*>(memrchr(buffer, '#', buffersize));
    if (lastHash) {
        // Common case optimization: Header with inclusion guard
        if (strncmp(lastHash, "#endif", 6) == 0) {
            lastHash = static_cast<const char*>(memrchr(buffer, '#', lastHash - buffer - 1));
        }
        if (lastHash) {
            const char* nextNewline = static_cast<const char*>(memchr(lastHash, '\n', buffersize - (lastHash - buffer)));
            if (nextNewline) {
                buffersize = nextNewline - buffer;
            }
        }
    }
    if (withLoc) {
        f.loc = 0;
        for (size_t n = 0; n < buffersize; n++) {
            if (buffer[n] == '\n') f.loc++;
        }
    }
    const char* nextHash = static_cast<const char*>(memchr(buffer+offset, '#', buffersize-offset));
    const char* nextSlash = static_cast<const char*>(memchr(buffer+offset, '/', buffersize-offset));
    size_t start = 0;
    while (offset < buffersize) {
        switch (state) {
        case None:
        {
            if (nextHash && nextHash < buffer + offset) nextHash = static_cast<const char*>(memchr(buffer+offset, '#', buffersize-offset));
            if (nextHash == NULL) return;
            if (nextSlash && nextSlash < buffer + offset) nextSlash = static_cast<const char*>(memchr(buffer+offset, '/', buffersize-offset));
            if (nextSlash && nextSlash < nextHash) {
                offset = nextSlash - buffer;
                if (buffer[offset + 1] == '/') {
                    offset = static_cast<const char*>(memchr(buffer+offset, '\n', buffersize-offset)) - buffer;
                }
                else if (buffer[offset + 1] == '*') {
                    do {
                        const char* nextSlash = static_cast<const char*>(memchr(buffer + offset + 1, '/', buffersize - offset));
                        if (!nextSlash) return;
                        offset = nextSlash - buffer;
                    } while (buffer[offset-1] != '*');
                }
            } else {
                offset = nextHash - buffer;
                state = AfterHash;
            }
        }
            break;
        case AfterHash:
            switch (buffer[offset]) {
            case ' ':
            case '\t':
                break;
            case 'i':
                if (buffer[offset + 1] == 'm' &&
                    buffer[offset + 2] == 'p' &&
                    buffer[offset + 3] == 'o' &&
                    buffer[offset + 4] == 'r' &&
                    buffer[offset + 5] == 't') {
                    state = AfterInclude;
                    offset += 5;
                }
                else if (buffer[offset + 1] == 'n' &&
                    buffer[offset + 2] == 'c' &&
                    buffer[offset + 3] == 'l' &&
                    buffer[offset + 4] == 'u' &&
                    buffer[offset + 5] == 'd' &&
                    buffer[offset + 6] == 'e') {
                    state = AfterInclude;
                    offset += 6;
                }
                else
                {
                    state = None;
                }
                break;
            default:
                state = None;
                break;
            }
            break;
        case AfterInclude:
            switch (buffer[offset]) {
            case ' ':
            case '\t':
                break;
            case '<':
                start = offset + 1;
                state = InsidePointyIncludeBrackets;
                break;
            case '"':
                start = offset + 1;
                state = InsideStraightIncludeBrackets;
                break;
            default:
                // Buggy code, skip over this include.
                state = None;
                break;
            }
            break;
        case InsidePointyIncludeBrackets:
            switch (buffer[offset]) {
            case '\n':
                state = None; // Buggy code, skip over this include.
                break;
            case '>':
                f.AddIncludeStmt(true, std::string(&buffer[start], &buffer[offset]));
                state = None;
                break;
            }
            break;
        case InsideStraightIncludeBrackets:
            switch (buffer[offset]) {
            case '\n':
                state = None; // Buggy code, skip over this include.
                break;
            case '\"':
                f.AddIncludeStmt(false, std::string(&buffer[start], &buffer[offset]));
                state = None;
                break;
            }
            break;
        }
        offset++;
    }
}

#ifdef WITH_MMAP
static void ReadCode(std::unordered_map<std::string, File>& files, const filesystem::path &path, bool withLoc) {
    File& f = files.insert(std::make_pair(path.generic_string(), File(path))).first->second;
    int fd = open(path.c_str(), O_RDONLY);
    size_t fileSize = filesystem::file_size(path);
    void* p = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ReadCodeFrom(f, static_cast<const char*>(p), fileSize, withLoc);
    munmap(p, fileSize);
    close(fd);
}
#else
static void ReadCode(std::unordered_map<std::string, File>& files, const filesystem::path &path, bool withLoc) {
    File& f = files.insert(std::make_pair(path.generic_string(), File(path))).first->second;
    std::string buffer;
    buffer.resize(filesystem::file_size(path));
    {
        streams::ifstream(path).read(&buffer[0], buffer.size());
    }
    ReadCodeFrom(f, buffer.data(), buffer.size(), withLoc);
}
#endif

static bool IsItemBlacklisted(const filesystem::path &path, const std::unordered_set<std::string> &ignorefiles) {
    // Add your own blacklisted items here.
    std::string file = path.filename().generic_string();
    return ignorefiles.find(file) != ignorefiles.end();
}

static void ReadCmakelist(std::unordered_map<std::string, Component *> &components,
                          const filesystem::path &path) {
    streams::ifstream in(path);
    std::string line;
    Component &comp = AddComponentDefinition(components, path.parent_path());
    do {
        getline(in, line);
        if (strstr(line.c_str(), Configuration::Get().regenTag.c_str())) {
            comp.recreate = true;
        }
        if (strstr(line.c_str(), "project(") == line.c_str()) {
            size_t end = line.find(')');
            if (end != line.npos) {
                comp.name = line.substr(8, end - 8);
            }
        } else if (strstr(line.c_str(), "_library")) {
            comp.type = "library";
        } else if (strstr(line.c_str(), "_executable")) {
            comp.type = "executable";
        }
    } while (in.good());
}

bool IsCompileableFile(const std::string& ext) {
    static const std::unordered_set<std::string> exts = { ".c", ".C", ".cc", ".cpp", ".m", ".mm" };
    return exts.count(ext) > 0;
}

static bool IsCode(const std::string &ext) {
    static const std::unordered_set<std::string> exts = { ".c", ".C", ".cc", ".cpp", ".m", ".mm", ".h", ".H", ".hpp" };
    return exts.count(ext) > 0;
}

#if 1
void LoadFileList(std::unordered_map<std::string, Component *> &components,
                  std::unordered_map<std::string, File>& files,
                  const std::unordered_set<std::string> &ignorefiles,
                  const filesystem::path& sourceDir,
                  bool inferredComponents,
                  bool withLoc) {
    filesystem::path outputpath = filesystem::current_path();
    filesystem::current_path(sourceDir.c_str());
    AddComponentDefinition(components, ".");
    for (filesystem::recursive_directory_iterator it("."), end;
         it != end; ++it) {
        const auto &parent = it->path().parent_path();
        if (inferredComponents) AddComponentDefinition(components, parent);

        // skip hidden files and dirs
        if (filesystem::is_directory(parent) &&
            parent.filename().generic_string().size() > 2 &&
            parent.filename().generic_string()[0] == '.') {
            it.pop();
            if (it == end) {
                break;
            }
        }

        if (IsItemBlacklisted(it->path(), ignorefiles)) {
          continue;
        }
        if (it->path().filename() == "CMakeLists.txt") {
            ReadCmakelist(components, it->path());
        } else if (filesystem::is_regular_file(it->status())) {
            if (it->path().generic_string().find("CMakeAddon.txt") != std::string::npos) {
                AddComponentDefinition(components, parent).hasAddonCmake = true;
            } else if (IsCode(it->path().extension().generic_string().c_str())) {
                ReadCode(files, it->path(), withLoc);
            }
        }
    }
    filesystem::current_path(outputpath);
}
#else
static void LoadFileListFrom(std::unordered_map<std::string, Component *> &components,
                      std::unordered_map<std::string, File>& files,
                      const std::unordered_set<std::string> &ignorefiles,
                      const filesystem::path& sourceDir,
                      bool inferredComponents) {
    static const std::string cmakelistfilename = "CMakeLists.txt";
    // skip hidden files and dirs
    if (sourceDir.filename().generic_string().size() > 2 &&
        sourceDir.filename().generic_string()[0] == '.') {
        return;
    }

    for (filesystem::directory_iterator it(sourceDir), end;
         it != end; ++it) {
        if (IsItemBlacklisted(it->path(), ignorefiles)) {
            continue;
        }
        const auto &parent = it->path().parent_path();
        if (inferredComponents) AddComponentDefinition(components, parent);

        if (it->path().filename() == cmakelistfilename) {
            ReadCmakelist(components, it->path());
        } else if (filesystem::is_regular_file(it->status())) {
            if (it->path().generic_string().find("CMakeAddon.txt") != std::string::npos) {
                AddComponentDefinition(components, parent).hasAddonCmake = true;
            } else if (IsCode(it->path().extension().generic_string().c_str())) {
                ReadCode(files, it->path(), withLoc);
            }
        } else if (filesystem::is_directory(it->status())) {
            LoadFileListFrom(components, files, ignorefiles, it->path(), inferredComponents);
        }
    }
}

void LoadFileList(std::unordered_map<std::string, Component *> &components,
                  std::unordered_map<std::string, File>& files,
                  const std::unordered_set<std::string> &ignorefiles,
                  const filesystem::path& sourceDir,
                  bool inferredComponents,
                  bool withLoc) {
    filesystem::path outputpath = filesystem::current_path();
    filesystem::current_path(sourceDir.c_str());
    AddComponentDefinition(components, ".");
    LoadFileListFrom(components, files, ignorefiles, sourceDir, inferredComponents);
    filesystem::current_path(outputpath);
}
#endif

