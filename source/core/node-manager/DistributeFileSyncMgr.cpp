#include "DistributeFileSyncMgr.h"
#include <iostream>

namespace sf1r
{

void DistributeFileSyncMgr::getNewestReqLog(uint32_t start_from, const std::string& savepath)
{
    std::cout << "get newest log file from primary and put them to redo path.";
}

void DistributeFileSyncMgr::getNewestSCDFileList(std::vector<std::string>& filelist)
{

}

bool DistributeFileSyncMgr::getFileFromPrimary(const std::string& filepath)
{
    return true;
}

}
