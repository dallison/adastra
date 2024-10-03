#include "common/parameters.h"
#include <gtest/gtest.h>

TEST(ParametersTest, SetBasic) {
  adastra::parameters::ParameterServer server;
  EXPECT_TRUE(server.SetParameter("/foo", 42).ok());
  EXPECT_TRUE(server.SetParameter("/bar", "baz").ok());
  EXPECT_TRUE(server.SetParameter("/baz", 3.14).ok());
  EXPECT_TRUE(server.SetParameter("/qux", true).ok());

  absl::StatusOr<std::vector<std::string>> names = server.ListParameters();
  EXPECT_TRUE(names.ok());
  EXPECT_EQ(names->size(), 4);
  // Print the names.
  for (const std::string &name : *names) {
    std::cerr << name << std::endl;
  }

  // Get the values.
  absl::StatusOr<adastra::parameters::Value> value =
      server.GetParameter("/foo");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetInt32(), 42);

  value = server.GetParameter("/bar");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetString(), "baz");

  value = server.GetParameter("/baz");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetDouble(), 3.14);

  value = server.GetParameter("/qux");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetBool(), true);
}

TEST(ParametersTest, SetNested1) {
  adastra::parameters::ParameterServer server;
  EXPECT_TRUE(server.SetParameter("/foo/bar", 42).ok());
  absl::StatusOr<adastra::parameters::Value> value =
      server.GetParameter("/foo/bar");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetInt32(), 42);
  // Get the value of "/foo".  This will be a map.
  value = server.GetParameter("/foo");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetType(), adastra::parameters::Type::kMap);
  EXPECT_EQ(value->GetMap().size(), 1);
  EXPECT_EQ(value->GetMap().at("bar").GetInt32(), 42);

  absl::StatusOr<std::vector<std::string>> names = server.ListParameters();
  EXPECT_TRUE(names.ok());
  EXPECT_EQ(names->size(), 1);
  // Print the names.
  for (const std::string &name : *names) {
    std::cerr << name << std::endl;
  }
}

TEST(ParametersTest, SetNested2) {
  adastra::parameters::ParameterServer server;
  EXPECT_TRUE(server.SetParameter("/foo/bar/baz", 42).ok());
  absl::StatusOr<adastra::parameters::Value> value =
      server.GetParameter("/foo/bar/baz");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetInt32(), 42);
  // Get the value of "/foo".  This will be a map.
  value = server.GetParameter("/foo");
  EXPECT_TRUE(value.ok());
  EXPECT_EQ(value->GetType(), adastra::parameters::Type::kMap);
  EXPECT_EQ(value->GetMap().size(), 1);
  std::cerr << *value << std::endl;
  absl::StatusOr<std::vector<std::string>> names = server.ListParameters();
  EXPECT_TRUE(names.ok());
  EXPECT_EQ(names->size(), 1);
  // Print the names.
  for (const std::string &name : *names) {
    std::cerr << name << std::endl;
  }
}

TEST(ParametersTest, SetMap1) {
  adastra::parameters::ParameterServer server;
  std::map<std::string, adastra::parameters::Value> map = {
      {"foo", 42}, {"bar", "baz"}, {"baz", 3.14}, {"qux", true},
  };
  EXPECT_TRUE(server.SetParameter("/foo", map).ok());

  absl::StatusOr<std::vector<std::string>> names = server.ListParameters();
  EXPECT_TRUE(names.ok());
  EXPECT_EQ(names->size(), 4);
  // Print the names.
  for (const std::string &name : *names) {
    std::cerr << name << std::endl;
  }
}

TEST(ParametersTest, Delete) {
  adastra::parameters::ParameterServer server;
  EXPECT_TRUE(server.SetParameter("/foo", 42).ok());
  EXPECT_TRUE(server.SetParameter("/bar", "baz").ok());
  EXPECT_TRUE(server.SetParameter("/baz", 3.14).ok());
  EXPECT_TRUE(server.SetParameter("/qux", true).ok());

  EXPECT_TRUE(server.HasParameter("/foo"));
  EXPECT_TRUE(server.DeleteParameter("/foo").ok());
  EXPECT_FALSE(server.HasParameter("/foo"));

  server.Dump(std::cerr);

  EXPECT_TRUE(server.SetParameter("/foo/bar/baz", 42).ok());
  EXPECT_TRUE(server.HasParameter("/foo/bar"));
  EXPECT_TRUE(server.HasParameter("/foo/bar/baz"));
  EXPECT_TRUE(server.DeleteParameter("/foo/bar/baz").ok());
  EXPECT_FALSE(server.HasParameter("/foo/bar/baz"));

  EXPECT_TRUE(server.HasParameter("/foo"));
  EXPECT_TRUE(server.DeleteParameter("/foo").ok());
  EXPECT_FALSE(server.HasParameter("/foo"));

  absl::StatusOr<std::vector<std::string>> names = server.ListParameters();
  EXPECT_TRUE(names.ok());
  // Print the names.
  for (const std::string &name : *names) {
    std::cerr << name << std::endl;
  }
}
