#pragma once

#include <utils.h>
#include <iostream>
#include <string>

namespace grand {

#define DO_ACT(succState) \
    if(ev.fun()) { \
        transit<succState>(); \
    } \
    else { \
        transit<SError>(); \
    }

typedef std::function<bool()> ActionFunction;

struct SIdle;
struct SInitialized;
struct SConfigured;
struct SOneConfigured;
struct SRunning;
struct SError;

struct ECommon : tinyfsm::Event {
    ActionFunction fun;
};

struct EInitialize : ECommon { };
struct EConfigure : ECommon { };
struct EConfigureOne : ECommon { };
struct EStart : ECommon { };
struct EStop : ECommon { };
struct ETerminate : ECommon { };
struct EToError: ECommon { };

struct DUState : tinyfsm::Fsm<DUState>
{
  virtual void react(EInitialize const &) { };
  virtual void react(EConfigure const &) { };
  virtual void react(EConfigureOne const &) { };
  virtual void react(EStart const &) { };
  virtual void react(EStop const &) { };
  virtual void react(ETerminate const &) { };
  virtual void react(EToError const &) { };

  virtual void entry(void) { };  /* entry actions in some states */
  virtual void exit(void) { };  /* no exit actions */

  virtual std::string stateString() { return "UNKOWN"; };

  static std::string state() {
    return current_state_ptr->stateString();
  }
};


// ----------------------------------------------------------------------------
// 3. State Declarations
//
struct SIdle : DUState
{
  void entry() override { 
    std::cout << "Du-daq softawre Version is 2023/03/01 in Dun Huang." << std::endl;
    std::cout << "Enter state: " << stateString() << std::endl; 
  }
  void react(EInitialize const &ev) override { DO_ACT(SInitialized); }
  void react(ETerminate const &ev) override { DO_ACT(SIdle); }
  
  std::string stateString() { return "IDLE"; }
};

struct SInitialized : DUState
{
  void entry() override { std::cout << "Enter state: " << stateString() << std::endl; }
  void react(EConfigure const &ev) override { DO_ACT(SConfigured); }
  void react(EConfigureOne const &ev) override { DO_ACT(SOneConfigured); }
  void react(ETerminate const &ev) override { DO_ACT(SIdle); }

  std::string stateString() { return "INITIALIZED"; }
};

struct SConfigured : DUState
{
  void entry() override { std::cout << "Enter state: " << stateString() << std::endl; }
  void react(EStart const &ev) override { DO_ACT(SRunning); }
  void react(ETerminate const &ev) override { DO_ACT(SIdle); }

  std::string stateString() { return "CONFIGURED"; }
};

struct SOneConfigured : DUState
{
  void entry() override { std::cout << "Enter state: " << stateString() << std::endl; }
  void react(EStart const &ev) override { DO_ACT(SRunning); }
  void react(ETerminate const &ev) override { DO_ACT(SIdle); }

  std::string stateString() { return "OneCONFIGURED"; }
};

struct SRunning : DUState
{
  void entry() override { std::cout << "Enter state: " << stateString() << std::endl; }
  void react(EStop const &ev) override { DO_ACT(SConfigured); }
  void react(ETerminate const &ev) override { DO_ACT(SIdle); }

  std::string stateString() { return "RUNNING"; }
};

struct SError : DUState
{
  void entry() override { std::cout << "Enter state: " << stateString() << std::endl; }
  void react(ETerminate const &ev) override { DO_ACT(SIdle); }

  std::string stateString() { return "ERROR"; }
};

class DUFSM
{
public: 
  static void start() {
    DUState::start();
  }

  static std::string state() {
    return DUState::state();
  }

  template<class EventType>
  static void sendEvent(EventType event) {
    DUState::dispatch(event);
  }
};

}
