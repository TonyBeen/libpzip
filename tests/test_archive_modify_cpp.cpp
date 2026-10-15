#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <string>
#include <vector>

#include "ghc/filesystem.hpp"
#include "pzip.h"

namespace fs = ghc::filesystem;

#define TEST_ASSERT(cond)                                                                     \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            return 1;                                                                         \
        }                                                                                     \
    } while (0)

namespace {

bool CreateDummyFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path.c_str(), std::ios::binary);
    if (!ofs.is_open()) {
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

std::string ReadFileContent(const std::string& path) {
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open()) {
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

struct ListContext {
    std::vector<std::string> names;
    std::vector<uint64_t> uncompressedSizes;
};

int32_t ListCallback(const pzip_entry_info_t* entry, void* userData) {
    ListContext* ctx = static_cast<ListContext*>(userData);
    if (entry != NULL && entry->entry_name != NULL) {
        ctx->names.push_back(entry->entry_name);
        ctx->uncompressedSizes.push_back(entry->uncompressed_size);
    }
    return 0;  // continue enumeration
}

}  // namespace

int32_t main() {
    const fs::path testDir = fs::current_path() / "pzip_test_archive_modify_workspace";
    std::error_code ec;
    fs::remove_all(testDir, ec);
    fs::create_directories(testDir, ec);

    const std::string fileA = (testDir / "fileA.txt").string();
    const std::string fileB = (testDir / "fileB.txt").string();
    const std::string fileC = (testDir / "fileC.txt").string();
    const std::string archivePath = (testDir / "test_modify.zip").string();

    TEST_ASSERT(CreateDummyFile(fileA, "Hello World from file A!"));
    TEST_ASSERT(CreateDummyFile(fileB, "Content of second file B with longer text."));
    TEST_ASSERT(CreateDummyFile(fileC, "File C payload for append test."));

    pzip_options_t opts;
    std::memset(&opts, 0, sizeof(opts));
    opts.abi_version = PZIP_ABI_VERSION;

    // Test 1: Create initial archive with fileA and fileB
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, archivePath.c_str(), PZIP_MODE_CREATE) == PZIP_OK);
        TEST_ASSERT(pzip_add_path(ctx, fileA.c_str(), NULL) == PZIP_OK);
        TEST_ASSERT(pzip_add_path(ctx, fileB.c_str(), NULL) == PZIP_OK);
        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Verify initial archive via pzip_list_archive
    {
        ListContext listCtx;
        TEST_ASSERT(pzip_list_archive(archivePath.c_str(), ListCallback, &listCtx) == PZIP_OK);
        TEST_ASSERT(listCtx.names.size() == 2);
        TEST_ASSERT(listCtx.names[0] == "fileA.txt");
        TEST_ASSERT(listCtx.names[1] == "fileB.txt");
    }

    // Test 2: Append Mode (PZIP_MODE_APPEND) - Append fileC
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, archivePath.c_str(), PZIP_MODE_APPEND) == PZIP_OK);

        uint32_t count = 0;
        TEST_ASSERT(pzip_get_entry_count(ctx, &count) == PZIP_OK);
        TEST_ASSERT(count == 2);

        int32_t existsA = 0;
        int32_t existsC = 0;
        TEST_ASSERT(pzip_has_entry(ctx, "fileA.txt", &existsA) == PZIP_OK);
        TEST_ASSERT(existsA == 1);
        TEST_ASSERT(pzip_has_entry(ctx, "fileC.txt", &existsC) == PZIP_OK);
        TEST_ASSERT(existsC == 0);

        TEST_ASSERT(pzip_add_path(ctx, fileC.c_str(), NULL) == PZIP_OK);
        TEST_ASSERT(pzip_get_entry_count(ctx, &count) == PZIP_OK);
        TEST_ASSERT(count == 3);

        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Verify 3 entries exist now
    {
        ListContext listCtx;
        TEST_ASSERT(pzip_list_archive(archivePath.c_str(), ListCallback, &listCtx) == PZIP_OK);
        TEST_ASSERT(listCtx.names.size() == 3);
        TEST_ASSERT(listCtx.names[0] == "fileA.txt");
        TEST_ASSERT(listCtx.names[1] == "fileB.txt");
        TEST_ASSERT(listCtx.names[2] == "fileC.txt");
    }

    // Test 3: Single Entry Extraction (pzip_extract_entry)
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        const std::string extractedC = (testDir / "extracted_fileC.txt").string();
        TEST_ASSERT(pzip_extract_entry(ctx, archivePath.c_str(), "fileC.txt", extractedC.c_str()) == PZIP_OK);
        TEST_ASSERT(ReadFileContent(extractedC) == "File C payload for append test.");

        // Non-existent entry extraction returns PZIP_E_NOT_FOUND
        const std::string nonExistentOut = (testDir / "non_existent.txt").string();
        TEST_ASSERT(pzip_extract_entry(ctx, archivePath.c_str(), "missing.txt", nonExistentOut.c_str()) == PZIP_E_NOT_FOUND);

        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Test 4: Delete fileB with recursive = 0
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, archivePath.c_str(), PZIP_MODE_MODIFY) == PZIP_OK);

        pzip_entry_info_t info;
        TEST_ASSERT(pzip_stat_entry(ctx, "fileB.txt", &info) == PZIP_OK);
        TEST_ASSERT(std::string(info.entry_name) == "fileB.txt");
        TEST_ASSERT(info.uncompressed_size == strlen("Content of second file B with longer text."));

        // Delete fileB
        TEST_ASSERT(pzip_remove_entry(ctx, "fileB.txt", 0) == PZIP_OK);

        int32_t existsB = 1;
        TEST_ASSERT(pzip_has_entry(ctx, "fileB.txt", &existsB) == PZIP_OK);
        TEST_ASSERT(existsB == 0);

        uint32_t count = 0;
        TEST_ASSERT(pzip_get_entry_count(ctx, &count) == PZIP_OK);
        TEST_ASSERT(count == 2);

        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Verify after deletion
    {
        ListContext listCtx;
        TEST_ASSERT(pzip_list_archive(archivePath.c_str(), ListCallback, &listCtx) == PZIP_OK);
        TEST_ASSERT(listCtx.names.size() == 2);
        TEST_ASSERT(listCtx.names[0] == "fileA.txt");
        TEST_ASSERT(listCtx.names[1] == "fileC.txt");
    }

    // Test 5: Rename fileA.txt -> renamedA.txt
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, archivePath.c_str(), PZIP_MODE_MODIFY) == PZIP_OK);
        TEST_ASSERT(pzip_rename_entry(ctx, "fileA.txt", "renamedA.txt") == PZIP_OK);

        int32_t existsOld = 1;
        int32_t existsNew = 0;
        TEST_ASSERT(pzip_has_entry(ctx, "fileA.txt", &existsOld) == PZIP_OK);
        TEST_ASSERT(existsOld == 0);
        TEST_ASSERT(pzip_has_entry(ctx, "renamedA.txt", &existsNew) == PZIP_OK);
        TEST_ASSERT(existsNew == 1);

        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Verify rename and extract renamedA
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        const std::string extRenamed = (testDir / "extracted_renamedA.txt").string();
        TEST_ASSERT(pzip_extract_entry(ctx, archivePath.c_str(), "renamedA.txt", extRenamed.c_str()) == PZIP_OK);
        TEST_ASSERT(ReadFileContent(extRenamed) == "Hello World from file A!");
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Test 6: Recursive directory removal (recursive != 0)
    const std::string dirArchive = (testDir / "dir_test.zip").string();
    const fs::path subFolder = testDir / "subfolder";
    const fs::path nestedFolder = subFolder / "nested";
    fs::create_directories(nestedFolder, ec);
    TEST_ASSERT(CreateDummyFile((subFolder / "doc1.txt").string(), "doc 1"));
    TEST_ASSERT(CreateDummyFile((nestedFolder / "deep.txt").string(), "deep file"));
    TEST_ASSERT(CreateDummyFile((testDir / "standalone.txt").string(), "standalone file"));

    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, dirArchive.c_str(), PZIP_MODE_CREATE) == PZIP_OK);
        TEST_ASSERT(pzip_add_path(ctx, subFolder.string().c_str(), "my_docs") == PZIP_OK);
        TEST_ASSERT(pzip_add_path(ctx, (testDir / "standalone.txt").string().c_str(), NULL) == PZIP_OK);
        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Delete "my_docs" recursively with recursive = 1
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, dirArchive.c_str(), PZIP_MODE_MODIFY) == PZIP_OK);

        uint32_t countBefore = 0;
        TEST_ASSERT(pzip_get_entry_count(ctx, &countBefore) == PZIP_OK);
        TEST_ASSERT(countBefore == 3);

        TEST_ASSERT(pzip_remove_entry(ctx, "my_docs", 1) == PZIP_OK);

        uint32_t countAfter = 0;
        TEST_ASSERT(pzip_get_entry_count(ctx, &countAfter) == PZIP_OK);
        TEST_ASSERT(countAfter == 1);

        int32_t existsDoc = 1;
        int32_t existsStandalone = 0;
        TEST_ASSERT(pzip_has_entry(ctx, "my_docs/doc1.txt", &existsDoc) == PZIP_OK);
        TEST_ASSERT(existsDoc == 0);
        TEST_ASSERT(pzip_has_entry(ctx, "standalone.txt", &existsStandalone) == PZIP_OK);
        TEST_ASSERT(existsStandalone == 1);

        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Verify after recursive delete
    {
        ListContext listCtx;
        TEST_ASSERT(pzip_list_archive(dirArchive.c_str(), ListCallback, &listCtx) == PZIP_OK);
        TEST_ASSERT(listCtx.names.size() == 1);
        TEST_ASSERT(listCtx.names[0] == "standalone.txt");
    }

    // Test 7: Encrypted Archive Modification (WinZip AES zero-decompression copy)
    const std::string encArchive = (testDir / "enc_test.zip").string();
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_set_password(ctx, "SecretPassword123!") == PZIP_OK);
        TEST_ASSERT(pzip_open_archive_mode(ctx, encArchive.c_str(), PZIP_MODE_CREATE) == PZIP_OK);
        TEST_ASSERT(pzip_add_path(ctx, fileA.c_str(), NULL) == PZIP_OK);
        TEST_ASSERT(pzip_add_path(ctx, fileB.c_str(), NULL) == PZIP_OK);
        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Delete fileA from encrypted archive without needing decompression of fileB!
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        // Note: No password needed just to delete fileA via stream-copy repack!
        TEST_ASSERT(pzip_open_archive_mode(ctx, encArchive.c_str(), PZIP_MODE_MODIFY) == PZIP_OK);
        TEST_ASSERT(pzip_remove_entry(ctx, "fileA.txt", 0) == PZIP_OK);
        TEST_ASSERT(pzip_run(ctx) == PZIP_OK);
        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Verify fileB is still fully encrypted and decryptable with the password!
    {
        pzip_ctx_t* ctx = NULL;
        TEST_ASSERT(pzip_create(&opts, &ctx) == PZIP_OK);
        TEST_ASSERT(pzip_set_password(ctx, "SecretPassword123!") == PZIP_OK);

        const std::string extEncB = (testDir / "extracted_enc_fileB.txt").string();
        TEST_ASSERT(pzip_extract_entry(ctx, encArchive.c_str(), "fileB.txt", extEncB.c_str()) == PZIP_OK);
        TEST_ASSERT(ReadFileContent(extEncB) == "Content of second file B with longer text.");

        TEST_ASSERT(pzip_destroy(ctx) == PZIP_OK);
    }

    // Cleanup
    fs::remove_all(testDir, ec);

    printf("All pzip archive modification tests passed successfully!\n");
    return 0;
}
