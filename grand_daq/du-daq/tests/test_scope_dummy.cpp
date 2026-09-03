
#include <scope_dummy.h>
#include <unistd.h>
#include <utils.h>

using namespace grand;

int main()
{
    LOG(INFO) << "test_scope_dummy start..";
    ScopeDummy scopeA;
    scopeA.initialize();
    scopeA.configure(nullptr);
    scopeA.start();
    sleep(1);
    scopeA.stop();
    return 0;
}
