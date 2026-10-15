#ifndef PZIP_ZIP_ZIP_READER_H_
#define PZIP_ZIP_ZIP_READER_H_

#include <stdint.h>
#include <fstream>
#include <string>
#include <vector>

#include "pzip.h"
#include "zip/zip_writer.h"

namespace pzip {

/**
 * @brief ZIP 归档读取器，负责 Central Directory 的逆向解析、条目定位与零解压流切片复制。
 */
class ZipReader {
   public:
    ZipReader();
    ~ZipReader();

    /**
     * @brief 打开指定路径的 ZIP 归档文件。
     * 
     * @param archivePath 归档文件的物理路径 (UTF-8)。
     * @return pzip_status_t PZIP_OK 成功；PZIP_E_IO 打开失败。
     */
    pzip_status_t open(const std::string& archivePath);

    /**
     * @brief 关闭已打开的文件句柄。
     */
    void close();

    /**
     * @brief 检查归档文件是否成功打开。
     */
    bool isOpen() const;

    /**
     * @brief 解析并提取归档所有的 Central Directory 记录。
     * 
     * @param[out] outRecords 输出 Central Directory 记录数组。
     * @return pzip_status_t PZIP_OK 成功；PZIP_E_NOT_SUPPORTED 格式不受支持或损坏。
     */
    pzip_status_t readCentralDirectory(std::vector<CentralRecord>* outRecords);

    /**
     * @brief 读取指定条目的原始未解密/未解压有效载荷字节数据。
     * 
     * @param[in]  record     目标条目的 CentralRecord。
     * @param[out] outPayload 输出原始压缩载荷数据。
     * @return pzip_status_t PZIP_OK 成功；I/O 错误返回相应状态码。
     */
    pzip_status_t readEntryPayload(const CentralRecord& record, std::vector<uint8_t>* outPayload);

    /**
     * @brief 将已有条目的完整切片 (局部头与原始数据) 零解压、零重压缩直接流拷贝写入 ZipWriter。
     * 
     * @param[in]  record           原归档中的条目记录。
     * @param[in]  effectiveName    若非空，将使用新名称重命名局部头中的文件名。
     * @param[in,out] writer        目标 ZipWriter 指针。
     * @param[out] outNewLocalOffset 输出在新归档中的局部头偏移量。
     * @return pzip_status_t PZIP_OK 成功；I/O 错误返回相应状态码。
     */
    pzip_status_t copyEntrySliceTo(const CentralRecord& record, const std::string& effectiveName,
                                   ZipWriter* writer, uint32_t* outNewLocalOffset);

    /**
     * @brief 静态快速枚举目标 ZIP 文件的条目元数据。
     * 
     * @param[in] archivePath 目标 ZIP 文件物理磁盘路径。
     * @param[in] callback    条目回调函数。
     * @param[in] userData    用户透传指针。
     * @return pzip_status_t PZIP_OK 成功。
     */
    static pzip_status_t listArchive(const std::string& archivePath,
                                     pzip_list_callback_t callback,
                                     void* userData);

    /**
     * @brief 获取底层输入文件流引用。
     */
    std::ifstream& stream() { return m_in; }

   private:
    pzip_status_t locateEocd(uint64_t* outEocdOffset, uint32_t* outCdOffset,
                             uint32_t* outCdSize, uint16_t* outTotalEntries);

    std::ifstream m_in;
    std::string m_path;
    uint64_t m_fileSize;
};

}  // namespace pzip

#endif  // PZIP_ZIP_ZIP_READER_H_
