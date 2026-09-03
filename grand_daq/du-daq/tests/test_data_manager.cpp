#include <data_manager.h>
#include <utils.h>
#include <cassert>

using namespace grand;

int main()
{
    DataManager dm;
    dm.initialize();
    bool bOut = false;
    dm.setEventOutput([&bOut](char *data, size_t sz)->void{
        LOG(INFO) << "output event, size = " << sz;
        bOut = true;
    });

    size_t size = 1024;
    char testData[size];

    int daqMode = 3;
    dm.addEvent(testData, size, daqMode);
    
    assert(bOut = true);

    dm.terminate();
    return 0;
}
