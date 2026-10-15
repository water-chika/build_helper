#pragma once

#include <unordered_map>
#include <memory>
#include <functional>
#include <filesystem>
#include <iostream>
#include <execution>
#include <unordered_set>
#include <cstring>
#include <cassert>
#include <string_view>
#include <format>
#include <optional>

namespace build {

uint64_t hash_file(const std::filesystem::path& file) {
    if (exists(file)) {
        auto t = last_write_time(file);
        auto duration = t.time_since_epoch();
        return duration.count();
    }
    else {
        return 0;
    }
}

enum class update_res : uint32_t {
    success = 0,
    failed = 1,
};

class file_object {
public:
    file_object(std::filesystem::path file,
            std::vector<std::weak_ptr<file_object>> dependencies = {},
            std::function<update_res(const file_object&)> update_action = {})
        : m_file{file},
        m_dependencies{dependencies},
        m_update_action{std::move(update_action)},
        m_dependencies_hash(dependencies.size()),
        m_hash{hash_file(file)}
    {
        update_dependencies_hash();
    }
    auto hash() const {
        return m_hash;
    }
    bool need_update() const {
        if (!exists(m_file)) {
            return true;
        }
        for (uint32_t i = 0; i < m_dependencies.size(); i++) {
            auto& dependency = m_dependencies[i];
            auto& dependency_prev_hash = m_dependencies_hash[i];
            if (auto shared_obj = dependency.lock()) {
                if (last_write_time(shared_obj->m_file) >= last_write_time(m_file)) {
                    return true;
                }
            }
        }

        return false;
    }
    update_res update() {
        auto res = m_update_action(*this);
        update_dependencies_hash();
        return res;
    }
    void add_dependency(std::weak_ptr<file_object> dependency) {
        m_dependencies.push_back(dependency);
        m_dependencies_hash.push_back({});
    }
    void add_dependency(std::vector<std::weak_ptr<file_object>> dependencies) {
        for (auto dependency : dependencies) {
            add_dependency(dependency);
        }
    }

    auto& get_file_path() const {
        return m_file;
    }
    auto& get_dependencies() const {
        return m_dependencies;
    }

private:
    void update_dependencies_hash() {
        for (uint32_t i = 0; i < m_dependencies.size(); i++) {
            auto& dependency = m_dependencies[i];
            auto& dependency_prev_hash = m_dependencies_hash[i];
            if (auto shared_obj = dependency.lock()) {
                dependency_prev_hash = shared_obj->hash();
            }
        }
    }
    std::filesystem::path m_file;
    std::vector<std::weak_ptr<file_object>> m_dependencies;
    std::function<update_res(const file_object&)> m_update_action;

    std::vector<uint64_t> m_dependencies_hash;

    uint64_t m_hash;
};

template<typename Object=file_object, typename Action = std::function<update_res(const file_object&)>>
class dependency_graph {
public:
    void add(std::shared_ptr<file_object> obj) {
        m_objects.push_back(obj);
    }
    update_res update() {
        bool is_updated = false;
        bool is_success = true;
        do {
            is_updated = false;
            std::for_each(
                std::execution::seq,
                m_objects.begin(), m_objects.end(),
                [&is_updated, &is_success](auto& object) {
                    if (object->need_update()) {
                        is_updated = true;
                        auto res = object->update();
                        if (res != update_res::success) {
                            is_success = false;
                        }
                    }
                });
        } while (is_updated && is_success);
        return is_success ? update_res::success : update_res::failed;
    }
private:
    std::vector<std::shared_ptr<file_object>> m_objects;
};

auto dependencies(auto... args) {
    return std::vector<std::weak_ptr<build::file_object>>{args...};
}

auto file(std::filesystem::path path, std::vector<std::weak_ptr<build::file_object>> dependencies = {}, std::function<update_res(const file_object&)> action = {}) {
    return std::make_shared<build::file_object>(
            path,
            dependencies,
            action
    );
}

// Out-of-source/separate-build-directory support: an output_path's parent
// directory (e.g. a build directory distinct from the source tree) may not
// exist yet; create it instead of letting the compiler/linker fail with
// "No such file or directory".
void ensure_parent_directory(const std::filesystem::path& output_path) {
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
}

auto c_plus_plus_compile(std::filesystem::path compiler, const std::vector<std::string>& src, const std::filesystem::path& output_path) {
    ensure_parent_directory(output_path);
    std::string sources = "";
    std::ranges::for_each(
            src,
            [&sources](auto s) {
                if (s.starts_with("-l") || s.starts_with("-L")) {
                    // Raw link flag passthrough, for system libraries that
                    // have no single canonical .so path (e.g. avformat is
                    // shipped under a versioned subdirectory on some
                    // distros), instead of requiring a real file on disk.
                    sources += " " + s;
                }
                else if (std::filesystem::is_directory(s)) {
                    sources += " -I" + s;
                }
                else if (s.ends_with(".hpp")) {}
                else if (s.ends_with(".so")) {
                    auto s_path = std::filesystem::path{s};
                    auto name = s_path.filename().string();
                    // Strip both the "lib" prefix (3 chars) and the ".so"
                    // suffix (3 chars), e.g. "libz.so" -> "z". The previous
                    // code only subtracted 3 once, leaving the ".so" suffix
                    // in place (e.g. "libz.so" -> "z.so"), which produces a
                    // bogus "-lz.so" link flag; amdclang_plus_plus_compile
                    // below already does this subtraction correctly.
                    auto link_name = std::string_view{name.data()+3, name.size()-6};
                    sources += " -l";
                    sources += link_name;
                }
                else if (s.ends_with(".cpp")) {
                    sources += " " + s;
                }
                else {
                    throw std::runtime_error{"not known cxx compile dependency"};
                }
            }
            );
    auto command = std::format("{} {} -o {} -std=c++23 -ltbb", compiler.string(), sources, output_path.string());
    std::cout << command << std::endl;
    return system(command.c_str());
}
auto c_plus_plus_compile(std::filesystem::path compiler, const std::string src, const std::filesystem::path& output_path) {
    return c_plus_plus_compile(compiler, std::vector<std::string>{src}, output_path);
}
auto amdclang_plus_plus_compile(std::filesystem::path compiler, const std::vector<std::string>& src, const std::filesystem::path& output_path) {
    ensure_parent_directory(output_path);
    std::string sources = "";
    std::ranges::for_each(
            src,
            [&sources](auto s) {
                if (std::filesystem::is_directory(s)) {
                    sources += " -I" + s;
                }
                else if (s.ends_with(".hpp")) {}
                else if (s.ends_with(".so")) {
                    auto s_path = std::filesystem::path{s};
                    auto name = s_path.filename().string();
                    auto link_name = std::string_view{name.data()+3, name.size()-3-3};
                    sources += " -l";
                    sources += link_name;
                }
                else if (s.ends_with(".cpp") || s.ends_with(".hip")) {
                    sources += " " + s;
                }
                else {
                    throw std::runtime_error{"not known hip compile dependency"};
                }
            }
            );
    auto command = std::format("{} -x hip --offload-arch=gfx1201 {} -o {} -std=c++23 -ltbb",
            compiler.string(), sources, output_path.string());
    std::cout << command << std::endl;
    return system(command.c_str());
}

// Delegates a subdirectory to its OWN existing CMakeLists.txt instead of
// hand-reproducing its build logic. For vendored, not-locally-maintained
// dependencies (e.g. GLFW, FreeType, SPIRV-Headers) that already have a
// working CMake build, reimplementing their compiler flags/codegen in
// build_helper is wasted, error-prone work; just configure+build them with
// real CMake, out-of-source, and link the resulting artifact. Configures
// once (skipped if build_dir/CMakeCache.txt already exists) and always
// re-runs the build step, so repeated calls are a cheap incremental build,
// not a reconfigure.
auto cmake_build(const std::filesystem::path& source_dir,
        const std::filesystem::path& build_dir,
        const std::string& target,
        const std::vector<std::string>& cmake_args = {}) {
    std::filesystem::create_directories(build_dir);
    if (!std::filesystem::exists(build_dir / "CMakeCache.txt")) {
        std::string args;
        for (auto& a : cmake_args) {
            args += " " + a;
        }
        auto configure_command = std::format("cmake -S {} -B {}{}",
                source_dir.string(), build_dir.string(), args);
        std::cout << configure_command << std::endl;
        if (system(configure_command.c_str()) != 0) {
            return 1;
        }
    }
    auto build_command = std::format("cmake --build {} --target {}",
            build_dir.string(), target);
    std::cout << build_command << std::endl;
    return system(build_command.c_str());
}

// Compiles ONE source file to an object file with its own compiler and
// flags, instead of c_plus_plus_compile/amdclang_plus_plus_compile's
// whole-program single-command model. This is what lets a HIP source
// (needs amdclang++ -x hip) and plain C/C++ sources (need the host
// compiler, and must NOT have -x hip forced onto them) coexist in one
// final executable, the same way CMake does it: each gets its own
// target/compile step, and only the link step combines them.
auto compile_object(std::filesystem::path compiler,
        const std::vector<std::string>& extra_flags,
        const std::filesystem::path& src,
        const std::filesystem::path& obj_output) {
    ensure_parent_directory(obj_output);
    std::string flags = "";
    for (auto& f : extra_flags) {
        flags += " " + f;
    }
    auto command = std::format("{}{} -c {} -o {}",
            compiler.string(), flags, src.string(), obj_output.string());
    std::cout << command << std::endl;
    return system(command.c_str());
}

// Links already-compiled object files (and raw -l/.so library deps, reusing
// the same classification as c_plus_plus_compile) into one executable, with
// a plain host linker invocation - no -x hip, no per-file language flags.
auto link_objects(std::filesystem::path linker,
        const std::vector<std::string>& objects_and_libs,
        const std::filesystem::path& output_path) {
    ensure_parent_directory(output_path);
    std::string args = "";
    std::ranges::for_each(
            objects_and_libs,
            [&args](auto s) {
                if (s.starts_with("-l") || s.starts_with("-L")) {
                    args += " " + s;
                }
                else if (s.ends_with(".so")) {
                    auto s_path = std::filesystem::path{s};
                    auto name = s_path.filename().string();
                    auto link_name = std::string_view{name.data()+3, name.size()-6};
                    args += " -l";
                    args += link_name;
                }
                else if (s.ends_with(".o")) {
                    args += " " + s;
                }
                else {
                    throw std::runtime_error{"not known link dependency: " + s};
                }
            });
    auto command = std::format("{} {} -o {}", linker.string(), args, output_path.string());
    std::cout << command << std::endl;
    return system(command.c_str());
}

struct cpp_file_compile_action {
    auto operator()(const auto& target){
        auto weak_sources = target.get_dependencies();
        auto sources = std::vector<std::shared_ptr<build::file_object>>(weak_sources.size());
        std::ranges::transform(
                weak_sources,
                sources.begin(),
                [](auto e) {
                    return e.lock();
                }
                );
        auto sources_string = std::vector<std::string>(sources.size());
        std::ranges::transform(
                sources,
                sources_string.begin(),
                [](auto& src) {
                    return src->get_file_path().string();
                }
                );
        auto res = c_plus_plus_compile(m_compiler, sources_string, target.get_file_path());
        return res == 0 ? build::update_res::success : build::update_res::failed;
    }

    std::filesystem::path m_compiler;
};
struct hip_file_compile_action {
    auto operator()(const auto& target){
        auto weak_sources = target.get_dependencies();
        auto sources = std::vector<std::shared_ptr<build::file_object>>(weak_sources.size());
        std::ranges::transform(
                weak_sources,
                sources.begin(),
                [](auto e) {
                    return e.lock();
                }
                );
        auto sources_string = std::vector<std::string>(sources.size());
        std::ranges::transform(
                sources,
                sources_string.begin(),
                [](auto& src) {
                    return src->get_file_path().string();
                }
                );
        auto res = amdclang_plus_plus_compile(m_compiler, sources_string, target.get_file_path());
        return res == 0 ? build::update_res::success : build::update_res::failed;
    }
    std::filesystem::path m_compiler;
};

#undef linux
enum class os {
    linux,
    win32
};
#ifdef _WIN32
constexpr auto current_os = os::win32;
constexpr auto path_sep = ';';
#else
constexpr auto current_os = os::linux;
constexpr auto path_sep = ':';
#endif

auto get_paths() {
    auto env_path =  std::getenv("PATH");
    auto paths = std::unordered_set<std::filesystem::path>{};

    uint32_t s = 0;
    while ('\0' != env_path[s]) {
        auto next = strchr(env_path+s, path_sep);
        if (next == NULL) {
            paths.emplace(std::string{env_path+s});
            break;
        }
        paths.emplace(std::string{env_path+s, next});
        s = (next - env_path)+1;
    }
    return paths;
}

std::optional<std::filesystem::path> get_command(std::string command) {
    auto paths = get_paths();
    for (auto& path : paths) {
        auto full_path = path / command;
        if (exists(full_path)) {
            return full_path;
        }
    }
    return std::nullopt;
}

auto find_cxx_compiler() {
    if (current_os == os::win32) {
        return "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
    }
    else {
        return "c++";
    }
}

std::string find_hip_compiler() {
    if (auto compiler_path = get_command("amdclang++")) {
        return compiler_path.value().string();
    }
    else if (exists(std::filesystem::path("/opt/rocm/bin/amdclang++"))) {
        return "/opt/rocm/bin/amdclang++";
    }
    assert(0);
}

class builder {
public:
    builder() : builder(std::filesystem::path{}) {
    }
    // build_directory: when set, every target's output path is placed under
    // this directory instead of wherever the caller's cwd happens to be
    // (CMake's out-of-source build-dir model), and the directory is created
    // up front. A relative path already starting with build_directory, or
    // any absolute path, is left as-is, so callers can still opt out
    // per-target.
    explicit builder(std::filesystem::path build_directory) :
        m_cxx_compiler{find_cxx_compiler()},
        m_hip_compiler{find_hip_compiler()},
        m_build_directory{std::move(build_directory)}
    {
        if (!m_build_directory.empty()) {
            std::filesystem::create_directories(m_build_directory);
        }
    }
    std::filesystem::path resolve_output_path(const std::filesystem::path& path) const {
        if (m_build_directory.empty() || path.is_absolute()) {
            return path;
        }
        return m_build_directory / path;
    }
    void add_executable_help(std::string name, std::filesystem::path path,
            auto compile_action,
            std::convertible_to<std::string> auto... sources) {
        path = resolve_output_path(path);
        auto sources_set = std::vector<std::string>{sources...};
        auto sources_refs = std::vector<std::weak_ptr<build::file_object>>(sources_set.size());
        for (uint32_t i = 0; i < sources_set.size(); i++) {
            auto& source = sources_set[i];
            if (!m_name_map.contains(source)) {
                m_name_map[source] = build::file(source);
            }
            sources_refs[i] = m_name_map[source];
        }
        if (m_name_map.contains(name)) {
            auto& exe = m_name_map[name];
            exe->add_dependency(sources_refs);
        }
        else {
            auto exe = build::file(path, build::dependencies(sources_refs),
                    std::move(compile_action)
                    );
            m_name_map[name] = exe;
            m_graph.add(exe);
        }
    }
    void add_executable(std::string name, std::filesystem::path path, std::convertible_to<std::string> auto... sources) {
        add_executable_help(name, path, build::cpp_file_compile_action{m_cxx_compiler}, sources...);
    }
    void add_hip_executable(std::string name, std::filesystem::path path, std::convertible_to<std::string> auto... sources) {
        add_executable_help(name, path, build::hip_file_compile_action{m_hip_compiler}, sources...);
    }
    void add_library(std::string name, std::filesystem::path path, std::convertible_to<std::string> auto... sources) {
        add_executable_help(name, path,
                [](const auto& n){
                    create_directory(n.get_file_path());
                    return build::update_res::success;
                },
                sources...);
    }
    void add_dependency(std::string name, std::string dir) {
        auto& target = m_name_map[name];
        if (!m_name_map.contains(dir)) {
            m_name_map[dir] = build::file(dir);
        }
        target->add_dependency(m_name_map[dir]);
    }
    void target_include_directory(std::string name, std::string dir) {
        add_dependency(name, dir);
    }
    void target_link_library(std::string name, std::string lib) {
        auto& target = m_name_map[name];
        auto& lib_target = m_name_map[lib];
        target->add_dependency(lib_target->get_dependencies());
    }
    auto build() {
        return m_graph.update();
    }
private:
    std::unordered_map<std::string, std::shared_ptr<build::file_object>> m_name_map;
    build::dependency_graph<> m_graph;
    std::filesystem::path m_cxx_compiler;
    std::filesystem::path m_hip_compiler;
    std::filesystem::path m_build_directory;
};

}
