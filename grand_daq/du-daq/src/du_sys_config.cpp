// #include "dudaq_app.h"
// #include <utils.h>
// #include <functional>
// #include <du_sys_config.h>
// #include <message_impl.h>

// using namespace std;
// using std::cout;
// using std::endl;

// using namespace grand;

// #define SYS_CONFIG_LOAD_GLOBAL(NAME, TYPE) m_duConfig.NAME = config["global"][#NAME].as<TYPE>()

// DUSysConfig *DUSysConfig::instance() {
//     static DUSysConfig self;
//     return &self;
// }

// DUSysConfig::DUSysConfig() {
// }

// void DUSysConfig::load(std::string file) {
//     YAML::Node config = YAML::LoadFile(file);
//     SYS_CONFIG_LOAD_GLOBAL(sendFrequence, uint32_t);
//     SYS_CONFIG_LOAD_GLOBAL(port, std::string);
//     SYS_CONFIG_LOAD_GLOBAL(daqMode, int);
// }