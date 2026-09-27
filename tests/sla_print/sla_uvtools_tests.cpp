#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include <boost/filesystem.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/UVtoolsArchive.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Sets an environment variable for one test and restores it afterwards.
struct EnvGuard {
    std::string name; bool had = false; std::string old;
    EnvGuard(const char *n, const std::string &value) : name(n) {
        if (const char *v = std::getenv(n)) { had = true; old = v; }
        ::setenv(n, value.c_str(), 1);
    }
    ~EnvGuard() { if (had) ::setenv(name.c_str(), old.c_str(), 1); else ::unsetenv(name.c_str()); }
};

// A folder whose name has a space and an accent, like real user folders.
fs::path scratch_dir() {
    fs::path d = fs::temp_directory_path() / fs::unique_path("uvtools test cabeça %%%%-%%%%");
    fs::create_directories(d);
    return d;
}

// Writes an executable shell script that stands in for UVtoolsCmd.
fs::path fake_uvtools(const std::string &body) {
    fs::path p = scratch_dir() / "UVtoolsCmd";
    std::ofstream(p.string()) << "#!/bin/sh\n" << body << "\n";
    fs::permissions(p, fs::perms::owner_all);
    return p;
}

std::string read_file(const fs::path &p) {
    std::ifstream in(p.string(), std::ios::binary);
    std::stringstream ss; ss << in.rdbuf(); return ss.str();
}

std::string error_of(const std::function<void()> &fn) {
    try { fn(); } catch (const ExportError &e) { return e.what(); }
    return "";
}

} // namespace

TEST_CASE("find_cmd uses UVTOOLS_CMD when it exists", "[uvtools]") {
    fs::path fake = fake_uvtools("exit 0");
    EnvGuard env("UVTOOLS_CMD", fake.string());
    REQUIRE(uvtools::find_cmd() == fake.string());
}

TEST_CASE("find_cmd reports a wrong UVTOOLS_CMD instead of falling back", "[uvtools]") {
    EnvGuard env("UVTOOLS_CMD", "/nonexistent/UVtoolsCmd");
    std::string msg = error_of([] { uvtools::find_cmd(); });
    REQUIRE(msg.find("UVTOOLS_CMD") != std::string::npos);
    REQUIRE(msg.find("/nonexistent/UVtoolsCmd") != std::string::npos);
}

TEST_CASE("convert passes convert <in> <encoder> <out> and handles spaces and accents", "[uvtools]") {
    fs::path fake = fake_uvtools("[ \"$1\" = convert ] || exit 9\n[ \"$3\" = GooFile ] || exit 8\ncp \"$2\" \"$4\"");
    fs::path dir = scratch_dir();
    fs::path in = dir / "peça de teste.sl1", out = dir / "saída final.goo";
    std::ofstream(in.string()) << "layers";
    uvtools::convert(fake.string(), in.string(), "GooFile", out.string());
    REQUIRE(read_file(out) == "layers");
}

TEST_CASE("convert reports exit code and UVtools output on failure", "[uvtools]") {
    fs::path fake = fake_uvtools("echo 'boom from uvtools' >&2\nexit 3");
    fs::path dir = scratch_dir();
    std::ofstream((dir / "a.sl1").string()) << "x";
    std::string msg = error_of([&] {
        uvtools::convert(fake.string(), (dir / "a.sl1").string(), "GooFile", (dir / "a.goo").string());
    });
    REQUIRE(msg.find("exit code 3") != std::string::npos);
    REQUIRE(msg.find("boom from uvtools") != std::string::npos);
}

TEST_CASE("convert treats exit 0 without an output file as failure", "[uvtools]") {
    fs::path fake = fake_uvtools("echo 'Unable to find a valid encoder from GooFile.' >&2\nexit 0");
    fs::path dir = scratch_dir();
    std::ofstream((dir / "a.sl1").string()) << "x";
    std::string msg = error_of([&] {
        uvtools::convert(fake.string(), (dir / "a.sl1").string(), "GooFile", (dir / "a.goo").string());
    });
    REQUIRE(msg.find("did not write") != std::string::npos);
    REQUIRE(msg.find("Unable to find a valid encoder") != std::string::npos);
}

TEST_CASE("convert does not hang on very large tool output", "[uvtools]") {
    fs::path fake = fake_uvtools("yes 'layer progress line' | head -n 200000\ncp \"$2\" \"$4\"");
    fs::path dir = scratch_dir();
    std::ofstream((dir / "a.sl1").string()) << "x";
    uvtools::convert(fake.string(), (dir / "a.sl1").string(), "GooFile", (dir / "a.goo").string());
    REQUIRE(fs::exists(dir / "a.goo"));
}

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/FileReader.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include <test_utils.hpp>

TEST_CASE("GOO and CXDLPV4 are registered as export-only formats", "[uvtools]") {
    for (auto [id, ext] : {std::pair{"GOO", "goo"}, std::pair{"CXDLPV4", "cxdlpv4"}}) {
        const ArchiveEntry *e = get_archive_entry(id);
        REQUIRE(e != nullptr);
        REQUIRE(std::string(e->ext) == ext);
        REQUIRE(e->wrfactoryfn);
        REQUIRE(!e->rdfactoryfn);
    }
}

// Slices the 20 mm test cube with the given archive format and exports it.
static void export_cube(const char *format, const fs::path &out) {
    SLAPrint print;
    SLAFullPrintConfig fullcfg;
    auto m = FileReader::load_model(TEST_DATA_DIR PATH_SEPARATOR + std::string("20mm_cube.obj"));
    fullcfg.printer_technology.setInt(ptSLA);
    fullcfg.set("sla_archive_format", format);
    fullcfg.set("supports_enable", false);
    fullcfg.set("pad_enable", false);
    DynamicPrintConfig cfg;
    cfg.apply(fullcfg);
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    print.apply(m, cfg);
    print.process();
    ThumbnailsList thumbnails;
    print.export_print(out.string(), thumbnails, "cube");
}

TEST_CASE("GOO export hands an SL1 archive to UVtools and saves its result", "[uvtools]") {
    // Fake converter copies the SL1 through, so the output must be a zip ("PK").
    // UVtools needs the output extension to match the encoder, so check that too.
    fs::path fake = fake_uvtools("[ \"$3\" = GooFile ] || exit 8\ncase \"$4\" in *.goo) ;; *) exit 7;; esac\ncp \"$2\" \"$4\"");
    EnvGuard env("UVTOOLS_CMD", fake.string());
    fs::path out = scratch_dir() / "cubo teste.goo";
    export_cube("GOO", out);
    REQUIRE(read_file(out).substr(0, 2) == "PK");
}

TEST_CASE("CXDLPV4 export uses the Creality encoder", "[uvtools]") {
    fs::path fake = fake_uvtools("[ \"$3\" = CrealityCXDLPv4File ] || exit 8\ncase \"$4\" in *.cxdlpv4) ;; *) exit 7;; esac\ncp \"$2\" \"$4\"");
    EnvGuard env("UVTOOLS_CMD", fake.string());
    fs::path out = scratch_dir() / "cube.cxdlpv4";
    export_cube("CXDLPV4", out);
    REQUIRE(fs::exists(out));
}

TEST_CASE("Failed conversion keeps an existing file untouched", "[uvtools]") {
    fs::path fake = fake_uvtools("echo 'encoder crashed' >&2\nexit 1");
    EnvGuard env("UVTOOLS_CMD", fake.string());
    fs::path out = scratch_dir() / "cube.cxdlpv4";
    std::ofstream(out.string()) << "previous good print";
    std::string msg = error_of([&] { export_cube("CXDLPV4", out); });
    REQUIRE(msg.find("encoder crashed") != std::string::npos);
    REQUIRE(msg.find("intermediate SL1 file was kept") != std::string::npos);
    REQUIRE(read_file(out) == "previous good print");
}

TEST_CASE("Missing UVtools fails before writing anything", "[uvtools]") {
    EnvGuard env("UVTOOLS_CMD", "/nonexistent/UVtoolsCmd");
    fs::path out = scratch_dir() / "cube.goo";
    std::string msg = error_of([&] { export_cube("GOO", out); });
    REQUIRE(msg.find("UVTOOLS_CMD") != std::string::npos);
    REQUIRE(!fs::exists(out));
}

#include "libslic3r/PresetBundle.hpp"

// The name for the archive of `format`, from `name_format`.
static std::string output_name(const char *format, const std::string &name_format)
{
    SLAPrint print;
    SLAFullPrintConfig fullcfg;
    fullcfg.printer_technology.setInt(ptSLA);
    fullcfg.set("sla_archive_format", format);
    fullcfg.set("output_filename_format", name_format);
    DynamicPrintConfig cfg;
    cfg.apply(fullcfg);
    Model m = FileReader::load_model(TEST_DATA_DIR PATH_SEPARATOR + std::string("20mm_cube.obj"));
    print.apply(m, cfg);
    return print.output_filename("Lacos");
}

TEST_CASE("File names get the extension of the archive format", "[uvtools]") {
    // Old presets and projects name every SLA file "[input_filename_base].sl1".
    CHECK(output_name("CXDLPV4", "[input_filename_base].sl1") == "Lacos.cxdlpv4");
    CHECK(output_name("GOO", "[input_filename_base].sl1") == "Lacos.goo");
    CHECK(output_name("SL1", "[input_filename_base].sl1") == "Lacos.sl1");
    // A name the user chose keeps its extension.
    CHECK(output_name("CXDLPV4", "[input_filename_base].zip") == "Lacos.zip");
}

TEST_CASE("The default SLA print preset names files after the archive format", "[uvtools]") {
    PresetBundle bundle;
    const std::string name_format = bundle.sla_prints.default_preset().config.opt_string("output_filename_format");
    CHECK(output_name("CXDLPV4", name_format) == "Lacos.cxdlpv4");
    CHECK(output_name("SL1", name_format) == "Lacos.sl1");
}
