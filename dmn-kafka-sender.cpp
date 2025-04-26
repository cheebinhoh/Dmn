// For study purpose, and copy from
// https://developer.confluent.io/get-started/c/

#include <iostream>
#include <sstream>
#include <string>

#include "kafka/dmn-kafka.hpp"

int main(int argc, char **argv) {
  dmn::Dmn_Kafka::ConfigType writeConfigs{};
  writeConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  writeConfigs["sasl.username"] = "C3T2TGVAQYYF7H6T";
  writeConfigs["sasl.password"] =
      "4JNbCdwNsK6HSgj65AHdZT5d9VsyWPX+lQysSPca70ehKN7uHsCuIyPlHw32gmNr";
  writeConfigs["security.protocol"] = "SASL_SSL";
  writeConfigs["sasl.mechanisms"] = "PLAIN";
  writeConfigs["acks"] = "all";
  writeConfigs[dmn::Dmn_Kafka::Topic] = "timer_counter";
  writeConfigs[dmn::Dmn_Kafka::Key] = "tick";

  dmn::Dmn_Kafka producer{dmn::Dmn_Kafka::Role::kProducer, writeConfigs};

  for (int i = 0; i < 10; i++) {
    std::stringstream os{};

    os << "heartbaet: " << i;

    producer.write(os.str());
  }

  return 0;
}
