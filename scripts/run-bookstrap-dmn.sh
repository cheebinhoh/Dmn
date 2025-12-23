#!/bin/bash

/usr/bin/start-kafka.sh &>/dev/null &

echo -n "start kafka broker"

i=0
while [ $i -lt 5 ]; do
   echo -n "."
   sleep 1
   i=$((i + 1))
done

echo

echo "creating kafka topic ..."
/opt/kafka/bin/kafka-topics.sh --create --topic timer_counter --bootstrap-server localhost:9092
