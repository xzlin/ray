// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "gtest/gtest.h"
#include "ray/gcs/gcs_server/test/gcs_server_test_util.h"
#include "ray/gcs/test/gcs_test_util.h"

namespace ray {

class GcsPlacementGroupSchedulerTest : public ::testing::Test {
 public:
  void SetUp() override {
    thread_io_service_.reset(new std::thread([this] {
      std::unique_ptr<boost::asio::io_service::work> work(
          new boost::asio::io_service::work(io_service_));
      io_service_.run();
    }));

    raylet_client_ = std::make_shared<GcsServerMocker::MockRayletResourceClient>();
    raylet_client1_ = std::make_shared<GcsServerMocker::MockRayletResourceClient>();
    gcs_table_storage_ = std::make_shared<gcs::InMemoryGcsTableStorage>(io_service_);
    gcs_pub_sub_ = std::make_shared<GcsServerMocker::MockGcsPubSub>(redis_client_);
    gcs_node_manager_ = std::make_shared<gcs::GcsNodeManager>(
        io_service_, io_service_, gcs_pub_sub_, gcs_table_storage_);
    gcs_table_storage_ = std::make_shared<gcs::InMemoryGcsTableStorage>(io_service_);
    store_client_ = std::make_shared<gcs::InMemoryStoreClient>(io_service_);
    gcs_placement_group_scheduler_ =
        std::make_shared<GcsServerMocker::MockedGcsPlacementGroupScheduler>(
            io_service_, gcs_table_storage_, *gcs_node_manager_,
            /*lease_client_fplacement_groupy=*/
            [this](const rpc::Address &address) {
              if (0 == address.port()) {
                return raylet_client_;
              } else {
                return raylet_client1_;
              }
            });
  }

  void TearDown() override {
    io_service_.stop();
    thread_io_service_->join();
  }

  template <typename Data>
  void WaitPendingDone(const std::vector<Data> &data, int expected_count) {
    auto condition = [this, &data, expected_count]() {
      absl::MutexLock lock(&vector_mutex_);
      return (int)data.size() == expected_count;
    };
    EXPECT_TRUE(WaitForCondition(condition, timeout_ms_.count()));
  }

  void AddNode(const std::shared_ptr<rpc::GcsNodeInfo> &node, int cpu_num = 10) {
    gcs_node_manager_->AddNode(node);
    rpc::HeartbeatTableData heartbeat;
    (*heartbeat.mutable_resources_available())["CPU"] = cpu_num;
    gcs_node_manager_->UpdateNodeRealtimeResources(ClientID::FromBinary(node->node_id()),
                                                   heartbeat);
  }

  void ReschedulingWhenNodeAddTest(rpc::PlacementStrategy strategy) {
    AddNode(Mocker::GenNodeInfo(0), 1);
    auto failure_handler =
        [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
          absl::MutexLock lock(&vector_mutex_);
          failure_placement_groups_.emplace_back(std::move(placement_group));
        };
    auto success_handler =
        [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
          absl::MutexLock lock(&vector_mutex_);
          success_placement_groups_.emplace_back(std::move(placement_group));
        };

    // Failed to schedule the placement group, because the node resources is not enough.
    auto request = Mocker::GenCreatePlacementGroupRequest("", strategy);
    auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);
    gcs_placement_group_scheduler_->Schedule(placement_group, failure_handler,
                                             success_handler);
    WaitPendingDone(failure_placement_groups_, 1);
    ASSERT_EQ(0, success_placement_groups_.size());

    // A new node is added, and the rescheduling is successful.
    AddNode(Mocker::GenNodeInfo(0), 2);
    gcs_placement_group_scheduler_->Schedule(placement_group, failure_handler,
                                             success_handler);
    ASSERT_TRUE(raylet_client_->GrantResourceReserve());
    ASSERT_TRUE(raylet_client_->GrantResourceReserve());
    WaitPendingDone(success_placement_groups_, 1);
  }

 protected:
  const std::chrono::milliseconds timeout_ms_{6000};
  absl::Mutex vector_mutex_;
  std::unique_ptr<std::thread> thread_io_service_;
  boost::asio::io_service io_service_;
  std::shared_ptr<gcs::StoreClient> store_client_;

  std::shared_ptr<GcsServerMocker::MockRayletResourceClient> raylet_client_;
  std::shared_ptr<GcsServerMocker::MockRayletResourceClient> raylet_client1_;
  std::shared_ptr<gcs::GcsNodeManager> gcs_node_manager_;
  std::shared_ptr<GcsServerMocker::MockedGcsPlacementGroupScheduler>
      gcs_placement_group_scheduler_;
  std::vector<std::shared_ptr<gcs::GcsPlacementGroup>> success_placement_groups_;
  std::vector<std::shared_ptr<gcs::GcsPlacementGroup>> failure_placement_groups_;
  std::shared_ptr<GcsServerMocker::MockGcsPubSub> gcs_pub_sub_;
  std::shared_ptr<gcs::GcsTableStorage> gcs_table_storage_;
  std::shared_ptr<gcs::RedisClient> redis_client_;
};

TEST_F(GcsPlacementGroupSchedulerTest, TestScheduleFailedWithZeroNode) {
  ASSERT_EQ(0, gcs_node_manager_->GetAllAliveNodes().size());
  auto request = Mocker::GenCreatePlacementGroupRequest();
  auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);

  // Schedule the placement_group with zero node.
  gcs_placement_group_scheduler_->Schedule(
      placement_group,
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        failure_placement_groups_.emplace_back(std::move(placement_group));
      },
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        success_placement_groups_.emplace_back(std::move(placement_group));
      });

  // The lease request should not be send and the scheduling of placement_group should
  // fail as there are no available nodes.
  ASSERT_EQ(raylet_client_->num_lease_requested, 0);
  ASSERT_EQ(0, success_placement_groups_.size());
  ASSERT_EQ(1, failure_placement_groups_.size());
  ASSERT_EQ(placement_group, failure_placement_groups_.front());
}

TEST_F(GcsPlacementGroupSchedulerTest, TestSchedulePlacementGroupSuccess) {
  auto node = Mocker::GenNodeInfo();
  AddNode(node);
  ASSERT_EQ(1, gcs_node_manager_->GetAllAliveNodes().size());

  auto request = Mocker::GenCreatePlacementGroupRequest();
  auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);

  // Schedule the placement_group with 1 available node, and the lease request should be
  // send to the node.
  gcs_placement_group_scheduler_->Schedule(
      placement_group,
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        absl::MutexLock lock(&vector_mutex_);
        failure_placement_groups_.emplace_back(std::move(placement_group));
      },
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        absl::MutexLock lock(&vector_mutex_);
        success_placement_groups_.emplace_back(std::move(placement_group));
      });

  ASSERT_EQ(2, raylet_client_->num_lease_requested);
  ASSERT_EQ(2, raylet_client_->lease_callbacks.size());
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  WaitPendingDone(failure_placement_groups_, 0);
  WaitPendingDone(success_placement_groups_, 1);
  ASSERT_EQ(placement_group, success_placement_groups_.front());
}

TEST_F(GcsPlacementGroupSchedulerTest, TestSchedulePlacementGroupFailed) {
  auto node = Mocker::GenNodeInfo();
  AddNode(node);
  ASSERT_EQ(1, gcs_node_manager_->GetAllAliveNodes().size());

  auto request = Mocker::GenCreatePlacementGroupRequest();
  auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);

  // Schedule the placement_group with 1 available node, and the lease request should be
  // send to the node.
  gcs_placement_group_scheduler_->Schedule(
      placement_group,
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        absl::MutexLock lock(&vector_mutex_);
        failure_placement_groups_.emplace_back(std::move(placement_group));
      },
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        absl::MutexLock lock(&vector_mutex_);
        success_placement_groups_.emplace_back(std::move(placement_group));
      });

  ASSERT_EQ(2, raylet_client_->num_lease_requested);
  ASSERT_EQ(2, raylet_client_->lease_callbacks.size());
  ASSERT_TRUE(raylet_client_->GrantResourceReserve(false));
  ASSERT_TRUE(raylet_client_->GrantResourceReserve(false));
  // Reply the placement_group creation request, then the placement_group should be
  // scheduled successfully.
  WaitPendingDone(failure_placement_groups_, 1);
  WaitPendingDone(success_placement_groups_, 0);
  ASSERT_EQ(placement_group, failure_placement_groups_.front());
}

TEST_F(GcsPlacementGroupSchedulerTest, TestSchedulePlacementGroupReturnResource) {
  auto node = Mocker::GenNodeInfo();
  AddNode(node);
  ASSERT_EQ(1, gcs_node_manager_->GetAllAliveNodes().size());

  auto request = Mocker::GenCreatePlacementGroupRequest();
  auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);

  // Schedule the placement_group with 1 available node, and the lease request should be
  // send to the node.
  gcs_placement_group_scheduler_->Schedule(
      placement_group,
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        absl::MutexLock lock(&vector_mutex_);
        failure_placement_groups_.emplace_back(std::move(placement_group));
      },
      [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
        absl::MutexLock lock(&vector_mutex_);
        success_placement_groups_.emplace_back(std::move(placement_group));
      });

  ASSERT_EQ(2, raylet_client_->num_lease_requested);
  ASSERT_EQ(2, raylet_client_->lease_callbacks.size());
  // One bundle success and the other failed.
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  ASSERT_TRUE(raylet_client_->GrantResourceReserve(false));
  ASSERT_EQ(1, raylet_client_->num_return_requested);
  // Reply the placement_group creation request, then the placement_group should be
  // scheduled successfully.
  WaitPendingDone(failure_placement_groups_, 1);
  WaitPendingDone(success_placement_groups_, 0);
  ASSERT_EQ(placement_group, failure_placement_groups_.front());
}

TEST_F(GcsPlacementGroupSchedulerTest, TestStrictPackStrategyBalancedScheduling) {
  AddNode(Mocker::GenNodeInfo(0));
  AddNode(Mocker::GenNodeInfo(1));
  auto failure_handler = [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
    absl::MutexLock lock(&vector_mutex_);
    failure_placement_groups_.emplace_back(std::move(placement_group));
  };
  auto success_handler = [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
    absl::MutexLock lock(&vector_mutex_);
    success_placement_groups_.emplace_back(std::move(placement_group));
  };

  // Schedule placement group, it will be evenly distributed over the two nodes.
  int select_node0_count = 0;
  int select_node1_count = 0;
  for (int index = 0; index < 10; ++index) {
    auto request =
        Mocker::GenCreatePlacementGroupRequest("", rpc::PlacementStrategy::STRICT_PACK);
    auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);
    gcs_placement_group_scheduler_->Schedule(placement_group, failure_handler,
                                             success_handler);

    if (!raylet_client_->lease_callbacks.empty()) {
      ASSERT_TRUE(raylet_client_->GrantResourceReserve());
      ASSERT_TRUE(raylet_client_->GrantResourceReserve());
      ++select_node0_count;
    } else {
      ASSERT_TRUE(raylet_client1_->GrantResourceReserve());
      ASSERT_TRUE(raylet_client1_->GrantResourceReserve());
      ++select_node1_count;
    }
  }
  WaitPendingDone(success_placement_groups_, 10);
  ASSERT_EQ(select_node0_count, 5);
  ASSERT_EQ(select_node1_count, 5);
}

TEST_F(GcsPlacementGroupSchedulerTest, TestStrictPackStrategyReschedulingWhenNodeAdd) {
  ReschedulingWhenNodeAddTest(rpc::PlacementStrategy::STRICT_PACK);
}

TEST_F(GcsPlacementGroupSchedulerTest, TestStrictPackStrategyResourceCheck) {
  auto node0 = Mocker::GenNodeInfo(0);
  AddNode(node0);
  auto failure_handler = [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
    absl::MutexLock lock(&vector_mutex_);
    failure_placement_groups_.emplace_back(std::move(placement_group));
  };
  auto success_handler = [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
    absl::MutexLock lock(&vector_mutex_);
    success_placement_groups_.emplace_back(std::move(placement_group));
  };
  auto request =
      Mocker::GenCreatePlacementGroupRequest("", rpc::PlacementStrategy::STRICT_PACK);
  auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);
  gcs_placement_group_scheduler_->Schedule(placement_group, failure_handler,
                                           success_handler);
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  WaitPendingDone(success_placement_groups_, 1);

  // Node1 has less number of bundles, but it doesn't satisfy the resource
  // requirement. In this case, the bundles should be scheduled on Node0.
  auto node1 = Mocker::GenNodeInfo(1);
  AddNode(node1, 1);
  gcs_placement_group_scheduler_->Schedule(placement_group, failure_handler,
                                           success_handler);
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  WaitPendingDone(success_placement_groups_, 2);
}

TEST_F(GcsPlacementGroupSchedulerTest, TestPackStrategyReschedulingWhenNodeAdd) {
  ReschedulingWhenNodeAddTest(rpc::PlacementStrategy::PACK);
}

TEST_F(GcsPlacementGroupSchedulerTest, TestPackStrategyLargeBundlesScheduling) {
  AddNode(Mocker::GenNodeInfo(0));
  AddNode(Mocker::GenNodeInfo(1));
  auto failure_handler = [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
    absl::MutexLock lock(&vector_mutex_);
    failure_placement_groups_.emplace_back(std::move(placement_group));
  };
  auto success_handler = [this](std::shared_ptr<gcs::GcsPlacementGroup> placement_group) {
    absl::MutexLock lock(&vector_mutex_);
    success_placement_groups_.emplace_back(std::move(placement_group));
  };

  // Schedule placement group which has large bundles.
  // One node does not have enough resources, so we will divide bundles to two nodes.
  auto request =
      Mocker::GenCreatePlacementGroupRequest("", rpc::PlacementStrategy::PACK, 15);
  auto placement_group = std::make_shared<gcs::GcsPlacementGroup>(request);
  gcs_placement_group_scheduler_->Schedule(placement_group, failure_handler,
                                           success_handler);
  RAY_CHECK(raylet_client_->num_lease_requested > 0);
  RAY_CHECK(raylet_client1_->num_lease_requested > 0);
  for (int index = 0; index < raylet_client_->num_lease_requested; ++index) {
    ASSERT_TRUE(raylet_client_->GrantResourceReserve());
  }
  for (int index = 0; index < raylet_client1_->num_lease_requested; ++index) {
    ASSERT_TRUE(raylet_client1_->GrantResourceReserve());
  }
  WaitPendingDone(success_placement_groups_, 1);
}

}  // namespace ray

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
