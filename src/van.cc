/**
 *  Copyright (c) 2015 by Contributors
 */
#include "ps/internal/van.h"
#include <thread>
#include <chrono>
#include "ps/base.h"
#include "ps/sarray.h"
#include "ps/internal/postoffice.h"
#include "ps/internal/customer.h"
#include "./network_utils.h"
#include "./meta.pb.h"
#include "./kafka_van.h"
#include "./resender.h"

namespace ps {

// interval in second between to heartbeast signals. 0 means no heartbeat.
// don't send heartbeast in default. because if the scheduler received a
// heartbeart signal from a node before connected to that node, then it could be
// problem.
static const int kDefaultHeartbeatInterval = 0;

Van* Van::Create(const std::string& type) {
  if (type == "kafka") {
    return new KAFKAVan();
  } else {
      LOG(FATAL) << "unsupported van type: " << type;
      return nullptr;
  }
}

void Van::ProcessTerminateCommand() {
  PS_VLOG(1) << my_node().ShortDebugString() << " is stopped";
  ready_ = false;
}

//
void Van::ProcessAddNodeCommandAtScheduler(
        Message* msg, Meta* nodes, Meta* recovery_nodes) {
  recovery_nodes->control.cmd = Control::ADD_NODE;
  time_t t = time(NULL);
  size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
  if (nodes->control.node.size() == num_nodes) {//all nodes are alive
    // sort the nodes according their ip and port,
    std::sort(nodes->control.node.begin(), nodes->control.node.end(),
              [](const Node& a, const Node& b) {
                  return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
              });
    // assign node rank
    for (auto& node : nodes->control.node) {
      std::string node_host_ip = node.hostname + ":" + std::to_string(node.port);
      if (connected_nodes_.find(node_host_ip) == connected_nodes_.end()) {// new node
        CHECK_EQ(node.id, Node::kEmpty);
        int id = node.role == Node::SERVER ?
                 Postoffice::ServerRankToID(num_servers_) :
                 Postoffice::WorkerRankToID(num_workers_);// new id?
        PS_VLOG(1) << "assign rank=" << id << " to node " << node.DebugString();
        node.id = id;
        //Connect(node); //gbxu
        Postoffice::Get()->UpdateHeartbeat(node.id, t);
        connected_nodes_[node_host_ip] = id;
      } else {
        int id = node.role == Node::SERVER ?
                 Postoffice::ServerRankToID(num_servers_) :
                 Postoffice::WorkerRankToID(num_workers_);// new id?
        shared_node_mapping_[id] = connected_nodes_[node_host_ip];
        node.id = connected_nodes_[node_host_ip];
      }
      if (node.role == Node::SERVER) num_servers_++;
      if (node.role == Node::WORKER) num_workers_++;
    }
    nodes->control.node.push_back(my_node_);
    nodes->control.cmd = Control::ADD_NODE;
    Message back;
    back.meta = *nodes;
    for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
      int recver_id = r;
      if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
        back.meta.recver = Postoffice::IDtoGroupID(recver_id);//add new node
        back.meta.timestamp = timestamp_++;
        Send(back);
      }
    }
    PS_VLOG(1) << "the scheduler is connected to "
               << num_workers_ << " workers and " << num_servers_ << " servers";
    ready_ = true;
  } else if (!recovery_nodes->control.node.empty()) {
    //TODO:need to recover?
    auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);
    std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
    // send back the recovery node
    CHECK_EQ(recovery_nodes->control.node.size(), 1);
    //Connect(recovery_nodes->control.node[0]); //?
    Postoffice::Get()->UpdateHeartbeat(recovery_nodes->control.node[0].id, t);
    Message back;
    for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
      if (r != recovery_nodes->control.node[0].id
          && dead_set.find(r) != dead_set.end()) {
        // do not try to send anything to dead node
        continue;
      }
      //r == recovery_nodes->control.node[0].id
      //or r not in dead_set
      // only send recovery_node to nodes already exist
      // but send all nodes to the recovery_node
      back.meta = (r == recovery_nodes->control.node[0].id) ? *nodes : *recovery_nodes;
      back.meta.recver = r;
      back.meta.timestamp = timestamp_++;
      Send(back);
    }
  }
}

void Van::UpdateLocalID(Message* msg, std::unordered_set<int>* deadnodes_set,
                        Meta* nodes, Meta* recovery_nodes) {
  auto& ctrl = msg->meta.control;
  int num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
  // assign an id
  if (msg->meta.sender == Meta::kEmpty) {//new node
    CHECK(is_scheduler_);
    CHECK_EQ(ctrl.node.size(), 1);
    if (nodes->control.node.size() < num_nodes) {
      nodes->control.node.push_back(ctrl.node[0]);//add the new node
    } else {
      // some node dies and restarts
      //gbxu: however nodes is empty??
      CHECK(ready_.load());
      for (size_t i = 0; i < nodes->control.node.size() - 1; ++i) {
        const auto& node = nodes->control.node[i];
        if (deadnodes_set->find(node.id) != deadnodes_set->end() &&
            node.role == ctrl.node[0].role) {
          auto& recovery_node = ctrl.node[0];
          // assign previous node id
          recovery_node.id = node.id;
          recovery_node.is_recovery = true;
          PS_VLOG(1) << "replace dead node " << node.DebugString()
                     << " by node " << recovery_node.DebugString();
          nodes->control.node[i] = recovery_node;
          recovery_nodes->control.node.push_back(recovery_node);
          break;
        }
      }
    }
  }
  // update my id
  for (size_t i = 0; i < ctrl.node.size(); ++i) {
    const auto& node = ctrl.node[i];
    if (my_node_.hostname == node.hostname && my_node_.port == node.port) {//gbxu
      if (getenv("DMLC_RANK") == nullptr) {
        my_node_ = node;//update the my_node_.id
        printf("INFO:%s\n",my_node_.DebugString().c_str());
        if(!is_scheduler_){
          StartConsumer();//wait the consumer, or it will loss msg //gbxu
          sleep(1);//TODO:optimization?
        }
        std::string rank = std::to_string(Postoffice::IDtoRank(node.id));
#ifdef _MSC_VER
        _putenv_s("DMLC_RANK", rank.c_str());
#else
        setenv("DMLC_RANK", rank.c_str(), true);
#endif
      }
    }
  }
}

void Van::ProcessHearbeat(Message* msg) {
  auto& ctrl = msg->meta.control;
  time_t t = time(NULL);
  for (auto &node : ctrl.node) {
    Postoffice::Get()->UpdateHeartbeat(node.id, t);
    if (is_scheduler_) {
      Message heartbeat_ack;
      heartbeat_ack.meta.recver = node.id;
      heartbeat_ack.meta.control.cmd = Control::HEARTBEAT;
      heartbeat_ack.meta.control.node.push_back(my_node_);
      heartbeat_ack.meta.timestamp = timestamp_++;
      // send back heartbeat
      Send(heartbeat_ack);
    }
  }
}

void Van::ProcessBarrierCommand(Message* msg) {
  auto& ctrl = msg->meta.control;
  /* ==================================dynamic add worker====================*/
  // if (!ready_.load()) {
  //   ready_ = true;
  //   return;
  // }
  /* ==================================dynamic add worker====================*/
  if (msg->meta.request) {//scheduler counts the number
    if (barrier_count_.empty()) {
      barrier_count_.resize(8, 0);
    }
    int group = ctrl.barrier_group;
    ++barrier_count_[group];
    PS_VLOG(1) << "Barrier count for " << group << " : " << barrier_count_[group];
    if (barrier_count_[group] ==
        static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size())) {
        //static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size())-1) {
        barrier_count_[group] = 0;
      Message res;
      res.meta.request = false;
      res.meta.app_id = msg->meta.app_id;
      res.meta.customer_id = msg->meta.customer_id;
      res.meta.control.cmd = Control::BARRIER;
      for (int r : Postoffice::Get()->GetNodeIDs(group)) {
        int recver_id = r;
        if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
          res.meta.recver = recver_id;
          res.meta.timestamp = timestamp_++;
          CHECK_GT(Send(res), 0);
        }
      }
    }
  } else {
    Postoffice::Get()->Manage(*msg);//scheduler open the barrier
  }
}

void Van::ProcessDataMsg(Message* msg) {
  // data msg
  /* ==================================dynamic add worker====================*/
  static std::queue<Message> msgs_wait_for_pull_reply;
  static int new_worker_count = 0;
  static int curr_epoch = 0;
  static int batch_per_epoch = 0;
  if (Postoffice::Get()->is_server()) {  
    if (Postoffice::IDtoRank(msg->meta.sender) >= Postoffice::Get()->num_workers() && msg->meta.simple_app == false) {
      // this is a new worker's pull
      // std::cerr << "server detacted a new worker\n";
      // std::cerr << msg->DebugString() << "\n";
      CHECK(msg->meta.push == false);
      CHECK(msg->meta.request == true);
      if (Postoffice::IDtoRank(msg->meta.sender) >= Postoffice::Get()->num_workers() + new_worker_count) {
        new_worker_count = Postoffice::IDtoRank(msg->meta.sender) + 1 - Postoffice::Get()->num_workers();
      }
      msgs_wait_for_pull_reply.push(std::move(*msg));
      return;
    }
  }
  /* ==================================dynamic add worker====================*/
  CHECK_NE(msg->meta.sender, Meta::kEmpty);
  CHECK_NE(msg->meta.recver, Meta::kEmpty);
  CHECK_NE(msg->meta.app_id, Meta::kEmpty);
  int app_id = msg->meta.app_id;
  int customer_id = Postoffice::Get()->is_worker() ? msg->meta.customer_id : app_id;
  // is worker: the msg customer id
  // scheduler or server: the msg app_id
  auto* obj = Postoffice::Get()->GetCustomer(app_id, customer_id, 5);
  CHECK(obj) << "timeout (5 sec) to wait App " << app_id << " customer " << customer_id \
    << " ready ";
//    if(msg->data.size()>0){
//        printf("van process:\n");
//        for(auto it:msg->data[0]){
//            printf("%d ",it);
//        }
//        if(msg->data.size()>1){
//            for(auto it:msg->data[1]){
//                printf(" %f ",it);
//            }
//            printf(" ending \n");
//        }
//    }
  obj->Accept(*msg);
  /* ==================================dynamic add worker====================*/
  if (Postoffice::Get()->is_server() && msg->meta.push == false && msg->meta.request == true && msg->meta.simple_app == false) {  
  // determine whether this is the last pull of one epoch
    if (msg->meta.last_pull && new_worker_count != 0) {

      // std::cerr << "is this server? received a msg with last_pull true!" << std::endl;
      // tell other old worker change num worker
      Message back;
      back.meta.control.cmd = Control::DYNAMIC_ADD_NODE;
      back.meta.timestamp = curr_epoch;
      back.meta.app_id = Postoffice::Get()->num_workers() + new_worker_count;
      // std::cerr << "server now send back dynamicaddnode with appid " << back.meta.app_id << "\n";
      back.meta.recver = 0;
      for (int i = 0; i < Postoffice::Get()->num_workers() + new_worker_count; i++) {
        back.meta.recver = Postoffice::WorkerRankToID(i);
        Send(back);
      }
      for (int i = 0; i < new_worker_count; i++) Postoffice::Get()->add_num_workers();

      new_worker_count = 0;
      while (!msgs_wait_for_pull_reply.empty()) {
        /* TODO: do connection here */
        Message t = std::move(msgs_wait_for_pull_reply.front());
        msgs_wait_for_pull_reply.pop();
        obj->Accept(t);
      }
    }
    //if (Postoffice::Get()->batch_per_epoch == -1 && Postoffice::IDtoRank(msg->meta.sender) == 0) {
      //batch_per_epoch++;
    //}
    if (msg->meta.last_pull && Postoffice::IDtoRank(msg->meta.sender) == 0) {
      // std::cerr << "server knows this is the last pull of epoch " << curr_epoch << "\n";
      Postoffice::Get()->set_curr_epoch(curr_epoch);
      curr_epoch++;
      // Postoffice::Get()->batch_per_epoch = batch_per_epoch;
      // std::cerr << "set batch_per_epoch to " << batch_per_epoch << std::endl;
    }
  }
  /* ==================================dynamic add worker====================*/
}

void Van::ProcessAddNodeCommand(Message* msg, Meta* nodes, Meta* recovery_nodes) {
  auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);
  std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
  auto& ctrl = msg->meta.control;

  UpdateLocalID(msg, &dead_set, nodes, recovery_nodes);

  if (is_scheduler_) {
    ProcessAddNodeCommandAtScheduler(msg, nodes, recovery_nodes);
  } else {
    //new node send the add_node info, then scheduler will send the info back.
    for (const auto& node : ctrl.node) {
      std::string addr_str = node.hostname + ":" + std::to_string(node.port);
      if (connected_nodes_.find(addr_str) == connected_nodes_.end()) {//new node
        //Connect(node);//gbxu:according the msg from scheduler, connect another workers or servers
        connected_nodes_[addr_str] = node.id;//include mysele
      }
      if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
      if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
    }
    PS_VLOG(1) << my_node_.ShortDebugString() << " is connected to others";
    if(my_node_.id != Node::kEmpty){
      ready_ = true;//wait until UpdateLocalID() get my_node_ id!
    }
  }
}

/* ==================================dynamic add worker====================*/
void Van::ProcessDynamicAddNodeCommand(Message* msg, Meta* nodes) {
  if (is_scheduler_) {
    std::cerr << "scheduler receives dynamic add node\n";
    // the following code is just like UpdateLocalID
    // numworker has not been changed
    CHECK(msg->meta.sender == Meta::kEmpty);
    CHECK_EQ(msg->meta.control.node.size(), 1);
    nodes->control.node.push_back(msg->meta.control.node[0]);
    auto& node = nodes->control.node.back();
    // the following code is just like ProcessAddNodeCommandAtScheduler
    time_t t = time(NULL);
    std::string node_host_ip = node.hostname + ":" + std::to_string(node.port);
    CHECK(connected_nodes_.find(node_host_ip) == connected_nodes_.end());
    CHECK_EQ(node.id, Node::kEmpty);
    // now we only support add worker
    CHECK(node.role == Node::WORKER);
    int id = Postoffice::WorkerRankToID(num_workers_);
    PS_VLOG(1) << "assign rank=" << id << " to node " << node.DebugString();
    node.id = id;
    Postoffice::Get()->UpdateHeartbeat(node.id, t);
    connected_nodes_[node_host_ip] = id;

    num_workers_++;
    Postoffice::Get()->add_num_workers();

    nodes->control.cmd = Control::DYNAMIC_ADD_NODE;
    Message back;
    back.meta = *nodes;
    int recver_id = id;
    // recver now listen to partition 0, send it to worker group
    back.meta.recver = Postoffice::IDtoGroupID(recver_id);
    back.meta.timestamp = timestamp_++;
    Send(back);
    PS_VLOG(1) << "the scheduler is connected to "
               << num_workers_ << " workers and " << num_servers_ << " servers";
    
  } else {
    if (my_node_.id != Node::kEmpty) {
      // a worker recvs ADD_NODE
      // count num of worker in the ctrl.node
      std::cerr << "worker recv add node from server, appid is " << msg->meta.app_id << " epo is " << msg->meta.timestamp << "\n";
      Postoffice::Get()->set_num_workers(msg->meta.app_id);
      Postoffice::Get()->set_curr_epoch(msg->meta.timestamp);
      // int cnt = 0;
      // for (auto & nd : msg->meta.control.node) {
      //   if (nd.role == Node::WORKER) cnt++;
      // }
      // if (cnt > Postoffice::Get()->num_workers()) {
      //   Postoffice::Get()->set_num_workers(cnt);
      // }
      return;
    }
    // the following code is just like UpdateLocalID
    // the new worker recvs scheduler's reply
    // std::cerr << "new worker receives dynamic add node back\n";
    CHECK(msg->meta.sender == kScheduler);
    for (size_t i = 0; i < msg->meta.control.node.size(); ++i) {
      const auto& node = msg->meta.control.node[i];
      if (my_node_.hostname == node.hostname && my_node_.port == node.port) {//gbxu
        if (getenv("DMLC_RANK") == nullptr) {
          my_node_ = node;//update the my_node_.id
          // debuger, to comment
          printf("INFO:%s\n",my_node_.DebugString().c_str());
          if(!is_scheduler_){
            StartConsumer();//wait the consumer, or it will loss msg //gbxu
            sleep(1);//TODO:optimization?
          }
          std::string rank = std::to_string(Postoffice::IDtoRank(node.id));
#ifdef _MSC_VER
          _putenv_s("DMLC_RANK", rank.c_str());
#else
          setenv("DMLC_RANK", rank.c_str(), true);
#endif
        }
      }
    }
    // the following code is just like ProcessAddNodeCommand
    // TODO: this seems useless???????
    //new node send the add_node info, then scheduler will send the info back.
    for (const auto& node : msg->meta.control.node) {
      std::string addr_str = node.hostname + ":" + std::to_string(node.port);
      if (connected_nodes_.find(addr_str) == connected_nodes_.end()) {//new node
        //Connect(node);//gbxu:according the msg from scheduler, connect another workers or servers
        connected_nodes_[addr_str] = node.id;//include mysele
      }
      if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
      if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
    }
    PS_VLOG(1) << my_node_.ShortDebugString() << " is connected to others";
    if(my_node_.id != Node::kEmpty){
      ready_ = true;//wait until UpdateLocalID() get my_node_ id!
      // TODO: tell up layer to pull for the first time
    }
  }
}
/* ==================================dynamic add worker====================*/

void Van::Start(int customer_id) {
  // get scheduler info
  start_mu_.lock();
  if (init_stage == 0) {
    scheduler_.hostname = std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));
    scheduler_.port = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
    scheduler_.role = Node::SCHEDULER;
    scheduler_.id = kScheduler;
    is_scheduler_ = Postoffice::Get()->is_scheduler();

    // get my node info
    if (is_scheduler_) {
      my_node_ = scheduler_;
    } else {
      auto role = Postoffice::Get()->is_worker() ? Node::WORKER : Node::SERVER;
      const char *nhost = Environment::Get()->find("DMLC_NODE_HOST");
      std::string ip;
      if (nhost) ip = std::string(nhost);
      if (ip.empty()) {
        const char *itf = Environment::Get()->find("DMLC_INTERFACE");
        std::string interface;
        if (itf) interface = std::string(itf);
        if (interface.size()) {
          GetIP(interface, &ip);
        } else {
          GetAvailableInterfaceAndIP(&interface, &ip);
        }
        CHECK(!interface.empty()) << "failed to get the interface";
      }
      int port = GetAvailablePort();
      const char *pstr = Environment::Get()->find("PORT");
      if (pstr) port = atoi(pstr);//fixed or not
      CHECK(!ip.empty()) << "failed to get ip";
      CHECK(port) << "failed to get a port";
      my_node_.hostname = ip;
      my_node_.role = role;
      my_node_.port = port;
      // cannot determine my id now, the scheduler will assign it later
      // set it explicitly to make re-register within a same process possible
      my_node_.id = Node::kEmpty;
    }

    /* -[gbxu
    // bind.
    my_node_.port = Bind(my_node_, is_scheduler_ ? 0 : 40);
    PS_VLOG(1) << "Bind to " << my_node_.DebugString();
    CHECK_NE(my_node_.port, -1) << "bind failed";
    // connect to the scheduler
    Connect(scheduler_);
     -]*/
    // ---[gbxy
    const char *brokers = Environment::Get()->find("BROKERS");
    Connect(brokers,TOSCHEDULER);//producer TOSCHEDULER
    Connect(brokers,TOSERVERS);//producer TOSERVERS
    Connect(brokers,TOWORKERS);//producer TOWORKERS
    if(is_scheduler_) {
      Bind(brokers,TOSCHEDULER);//consumer TOSCHEDULER
    }else if(Postoffice::Get()->is_worker()) {
      Bind(brokers,TOWORKERS);//consumer TOWORKERS
    } else {
      Bind(brokers,TOSERVERS);//consumer TOSERVERS
    }
    // ---]

    // for debug use
    if (Environment::Get()->find("PS_DROP_MSG")) {
      drop_rate_ = atoi(Environment::Get()->find("PS_DROP_MSG"));
    }
    // start receiver
    receiver_thread_ = std::unique_ptr<std::thread>(
            new std::thread(&Van::Receiving, this));
    init_stage++;
  }
  start_mu_.unlock();

  sleep(1);//wait for receiving gbxu TODO:optimization??
  if (!is_scheduler_) {
    // let the scheduler know myself
    Message msg;
    Node customer_specific_node = my_node_;
    customer_specific_node.customer_id = customer_id;
    msg.meta.recver = kScheduler;
    /*=================================dynamic add node===========================================*/
    if (Environment::Get()->find("DYNAMIC_ADD_NODE") && (std::string(Environment::Get()->find("DYNAMIC_ADD_NODE")) == "true")) {
      std::cerr << "start with dynamic add node, sending msg to scheduler\n";
      msg.meta.control.cmd = Control::DYNAMIC_ADD_NODE;
    } else {
      msg.meta.control.cmd = Control::ADD_NODE;
    }
    /*=================================dynamic add node===========================================*/
    msg.meta.control.node.push_back(customer_specific_node);
    msg.meta.timestamp = timestamp_++;
    Send(msg);
  }
    // wait until ready
  while (!ready_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  start_mu_.lock();
  if (init_stage == 1) {//pass by now
    // resender
    if (Environment::Get()->find("PS_RESEND") && atoi(Environment::Get()->find("PS_RESEND")) != 0) {
      int timeout = 1000;
      if (Environment::Get()->find("PS_RESEND_TIMEOUT")) {
        timeout = atoi(Environment::Get()->find("PS_RESEND_TIMEOUT"));
      }
      resender_ = new Resender(timeout, 10, this);
    }

    if (!is_scheduler_) {
      // start heartbeat thread
      heartbeat_thread_ = std::unique_ptr<std::thread>(
              new std::thread(&Van::Heartbeat, this));
    }
    init_stage++;
  }
  start_mu_.unlock();
}

void Van::Stop() {
  // stop threads
  Message exit;
  exit.meta.control.cmd = Control::TERMINATE;
  exit.meta.recver = my_node_.id;
  // only customer 0 would call this method
  exit.meta.customer_id = 0;
  // ---[gbxu

  //ProcessTerminateCommand();
  int ret = SendMsg(exit);
  CHECK_NE(ret, -1);
  // ---]
  receiver_thread_->join();

  if (!is_scheduler_) heartbeat_thread_->join();
  if (resender_) delete resender_;
}

int Van::Send(Message& msg) {
  int send_bytes = SendMsg(msg);
  CHECK_NE(send_bytes, -1);
  send_bytes_ += send_bytes;
  if (resender_) resender_->AddOutgoing(msg);
  if (Postoffice::Get()->verbose() >= 2) {
    PS_VLOG(2) << msg.DebugString();
  }
  return send_bytes;
}

void Van::Receiving() {
  Meta nodes;
  Meta recovery_nodes;  // store recovery nodes
  recovery_nodes.control.cmd = Control::ADD_NODE;

  while (true) {
    Message msg;
    int recv_bytes = RecvMsg(&msg);
    // For debug, drop received message
    if (ready_.load() && drop_rate_ > 0) {
      unsigned seed = time(NULL) + my_node_.id;
      if (rand_r(&seed) % 100 < drop_rate_) {
        LOG(WARNING) << "Drop message " << msg.DebugString();
        continue;
      }
    }

    CHECK_NE(recv_bytes, -1);
    recv_bytes_ += recv_bytes;
    if (Postoffice::Get()->verbose() >= 2) {
      PS_VLOG(2) << msg.DebugString();
    }
    // duplicated message
    if (resender_ && resender_->AddIncomming(msg)) continue;

    if (!msg.meta.control.empty()) {

      // control msg
      auto& ctrl = msg.meta.control;
      if (ctrl.cmd == Control::TERMINATE) {
        ProcessTerminateCommand();
        break;
      } else if (ctrl.cmd == Control::ADD_NODE) {
        ProcessAddNodeCommand(&msg, &nodes, &recovery_nodes);
      } else if (ctrl.cmd == Control::BARRIER) {
        ProcessBarrierCommand(&msg);
      } else if (ctrl.cmd == Control::HEARTBEAT) {
        ProcessHearbeat(&msg);
      /* ================================= dynamic add node =====================*/
      } else if (ctrl.cmd == Control::DYNAMIC_ADD_NODE) {
        ProcessDynamicAddNodeCommand(&msg, &nodes);
      /* ================================= dynamic add node =====================*/
      } else {
        LOG(WARNING) << "Drop unknown typed message " << msg.DebugString();
      }
    } else {
      ProcessDataMsg(&msg);
    }
  }
}

void Van::PackMeta(const Meta& meta, char** meta_buf, int* buf_size) {
  // convert into protobuf
  PBMeta pb;
  pb.set_head(meta.head);
  if (meta.app_id != Meta::kEmpty) pb.set_app_id(meta.app_id);
  if (meta.timestamp != Meta::kEmpty) pb.set_timestamp(meta.timestamp);
  if (meta.body.size()) pb.set_body(meta.body);
  if (meta.sender != Meta::kEmpty) pb.set_sender(meta.sender);//gbxu
  pb.set_push(meta.push);
  pb.set_request(meta.request);
  pb.set_simple_app(meta.simple_app);

  /* ==================================dynamic add worker====================*/
  pb.set_last_pull(meta.last_pull);
  /* ==================================dynamic add worker====================*/
  pb.set_customer_id(meta.customer_id);
  for (auto d : meta.data_type) pb.add_data_type(d);
  if (!meta.control.empty()) {
    auto ctrl = pb.mutable_control();
    ctrl->set_cmd(meta.control.cmd);
    if (meta.control.cmd == Control::BARRIER) {
      ctrl->set_barrier_group(meta.control.barrier_group);
    } else if (meta.control.cmd == Control::ACK) {
      ctrl->set_msg_sig(meta.control.msg_sig);
    }
    for (const auto& n : meta.control.node) {
      auto p = ctrl->add_node();
      p->set_id(n.id);
      p->set_role(n.role);
      p->set_port(n.port);
      p->set_hostname(n.hostname);
      p->set_is_recovery(n.is_recovery);
      p->set_customer_id(n.customer_id);
    }
  }

  // to string
  *buf_size = pb.ByteSize();
  //*meta_buf = new char[*buf_size+1];
  CHECK(pb.SerializeToArray(*meta_buf, *buf_size))
    << "failed to serialize protbuf";

}

void Van::UnpackMeta(const char* meta_buf, int buf_size, Meta* meta) {
  // to protobuf
//    printf("unpack:");
//  for (int i = 0; i < buf_size; i++) {
//    printf("%x,", (meta_buf)[i]);
//  }
//  printf("%d\n", buf_size);
  PBMeta pb;
  CHECK(pb.ParseFromArray(meta_buf, buf_size))
    << "failed to parse string into protobuf";

  // to meta
  meta->head = pb.head();
  meta->app_id = pb.has_app_id() ? pb.app_id() : Meta::kEmpty;
  meta->timestamp = pb.has_timestamp() ? pb.timestamp() : Meta::kEmpty;
  meta->request = pb.request();
  meta->push = pb.push();
  meta->simple_app = pb.simple_app();
  /* ==================================dynamic add worker====================*/
  meta->last_pull = pb.last_pull();
  /* ==================================dynamic add worker====================*/
  meta->body = pb.body();
  meta->customer_id = pb.customer_id();
  meta->sender =  pb.has_sender() ? pb.sender() : Meta::kEmpty;//gbxu
  meta->data_type.resize(pb.data_type_size());
  for (int i = 0; i < pb.data_type_size(); ++i) {
    meta->data_type[i] = static_cast<DataType>(pb.data_type(i));
  }
  if (pb.has_control()) {
    const auto& ctrl = pb.control();
    meta->control.cmd = static_cast<Control::Command>(ctrl.cmd());
    meta->control.barrier_group = ctrl.barrier_group();
    meta->control.msg_sig = ctrl.msg_sig();
    for (int i = 0; i < ctrl.node_size(); ++i) {
      const auto& p = ctrl.node(i);
      Node n;
      n.role = static_cast<Node::Role>(p.role());
      n.port = p.port();
      n.hostname = p.hostname();
      n.id = p.has_id() ? p.id() : Node::kEmpty;
      n.is_recovery = p.is_recovery();
      n.customer_id = p.customer_id();
      meta->control.node.push_back(n);
    }
  } else {
    meta->control.cmd = Control::EMPTY;
  }
}

void Van::Heartbeat() {
  const char* val = Environment::Get()->find("PS_HEARTBEAT_INTERVAL");
  const int interval = val ? atoi(val) : kDefaultHeartbeatInterval;
  if(interval){
      printf("start the heartbear\n\n");
  }
  while (interval > 0 && ready_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    Message msg;
    msg.meta.sender = my_node_.id;
    msg.meta.recver = kScheduler;
    msg.meta.control.cmd = Control::HEARTBEAT;
    msg.meta.control.node.push_back(my_node_);
    msg.meta.timestamp = timestamp_++;
    Send(msg);
  }
}
}  // namespace ps
