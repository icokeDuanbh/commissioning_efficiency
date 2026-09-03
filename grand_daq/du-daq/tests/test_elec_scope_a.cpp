//**********************************//
//******Created by duanbh,
//******2022/4/22
//**********************************//
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <cassert>
#include <scope_a.h>
#include <fstream>
#include <utils.h>

using namespace grand;

#define SHADOW_SIZE 200000

int main()
{
    char sl[SHADOW_SIZE];
    std::ifstream inF;
    LOG(INFO) << "Loading /tmp/shadow_list.dat..";
    inF.open("/tmp/shadow_list.dat", std::ifstream::binary);
    inF.read(sl, SHADOW_SIZE);
    inF.close();

    //for(int i=0; i<500; i++) {
    //    std::cout << (int)sl[i] << std::endl;
    //}

    LOG(INFO) << "Creating Scope object..";
    ScopeA scopeA;
    LOG(INFO) << "Initializing Scope..";
    scopeA.initialize();
    LOG(INFO) << "Configure Scope..";
    scopeA.configure(sl);
    LOG(INFO) << "Starting Scope..";
    scopeA.start();
    sleep(10);
    LOG(INFO) << "Stopping Scope..";
    scopeA.stop();
    LOG(INFO) << "Done!";
    return 0;
}
