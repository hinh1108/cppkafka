#ifndef CPPKAFKA_BUFFERED_PRODUCER_H
#define CPPKAFKA_BUFFERED_PRODUCER_H

#include <string>
#include <vector>
#include <type_traits>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <boost/optional.hpp>
#include "../producer.h"
#include "../message.h"

namespace cppkafka {

template <typename BufferType>
class BufferedProducer {
public:
    /**
     * Concrete builder
     */
    using Builder = ConcreteMessageBuilder<BufferType>;

    /**
     * \brief Constructs a buffered producer using the provided configuration
     *
     * \param config The configuration to be used on the actual Producer object
     */
    BufferedProducer(Configuration config);

    /**
     * \brief Adds a message to the producer's buffer. 
     *
     * The message won't be sent until flush is called.
     *
     * \param builder The builder that contains the message to be added
     */
    void add_message(const MessageBuilder& builder);

    /**
     * \brief Adds a message to the producer's buffer. 
     *
     * The message won't be sent until flush is called.
     *
     * Using this overload, you can avoid copies and construct your builder using the type
     * you are actually using in this buffered producer.
     *
     * \param builder The builder that contains the message to be added
     */
    void add_message(Builder builder);

    /**
     * \brief Flushes the buffered messages.
     *
     * This will send all messages and keep waiting until all of them are acknowledged.
     */
    void flush();

    /**
     * Gets the Producer object
     */
    Producer& get_producer();

    /**
     * Gets the Producer object
     */
    const Producer& get_producer() const;

    /**
     * Simple helper to construct a builder object
     */
    Builder make_builder(const Topic& topic);
private:
    // Pick the most appropriate index type depending on the platform we're using
    using IndexType = std::conditional<sizeof(void*) == 8, uint64_t, uint32_t>::type;

    template <typename BuilderType>
    void do_add_message(BuilderType&& builder);
    const Topic& get_topic(const std::string& topic);
    void produce_message(IndexType index, Builder& message);
    Configuration prepare_configuration(Configuration config);
    void on_delivery_report(const Message& message);

    Producer producer_;
    std::map<IndexType, Builder> messages_;
    std::vector<IndexType> failed_indexes_;
    IndexType current_index_{0};
    std::vector<Topic> topics_;
    std::unordered_map<std::string, unsigned> topic_mapping_;
};

template <typename BufferType>
BufferedProducer<BufferType>::BufferedProducer(Configuration config)
: producer_(prepare_configuration(std::move(config))) {

}

template <typename BufferType>
void BufferedProducer<BufferType>::add_message(const MessageBuilder& builder) {
    do_add_message(builder);
}

template <typename BufferType>
void BufferedProducer<BufferType>::add_message(Builder builder) {
    do_add_message(move(builder));
}

template <typename BufferType>
void BufferedProducer<BufferType>::flush() {
    for (auto& message_pair : messages_) {
        produce_message(message_pair.first, message_pair.second);
    }

    while (!messages_.empty()) {
        producer_.poll();
        if (!failed_indexes_.empty()) {
            for (const IndexType index : failed_indexes_) {
                produce_message(index, messages_.at(index));
            }
        }
        failed_indexes_.clear();
    }
}

template <typename BufferType>
template <typename BuilderType>
void BufferedProducer<BufferType>::do_add_message(BuilderType&& builder) {
    Builder local_builder(get_topic(builder.topic().get_name()));
    local_builder.partition(builder.partition());
    local_builder.key(std::move(builder.key()));
    local_builder.payload(std::move(builder.payload()));

    IndexType index = messages_.size();
    messages_.emplace(index, std::move(local_builder));
}

template <typename BufferType>
Producer& BufferedProducer<BufferType>::get_producer() {
    return producer_;
}

template <typename BufferType>
const Producer& BufferedProducer<BufferType>::get_producer() const {
    return producer_;
}

template <typename BufferType>
typename BufferedProducer<BufferType>::Builder
BufferedProducer<BufferType>::make_builder(const Topic& topic) {
    return Builder(topic);
}

template <typename BufferType>
const Topic& BufferedProducer<BufferType>::get_topic(const std::string& topic) {
    auto iter = topic_mapping_.find(topic);
    if (iter == topic_mapping_.end()) {
        unsigned index = topics_.size();
        topics_.push_back(producer_.get_topic(topic));
        iter = topic_mapping_.emplace(topic, index).first;
    }
    return topics_[iter->second];
}

template <typename BufferType>
void BufferedProducer<BufferType>::produce_message(IndexType index, Builder& builder) {
    bool sent = false;
    MessageBuilder local_builder(builder.topic());
    local_builder.partition(builder.partition())
                 .key(builder.key())
                 .payload(builder.payload())
                 .user_data(reinterpret_cast<void*>(index));
    while (!sent) {
        try {
            producer_.produce(local_builder);
            sent = true;
        }
        catch (const HandleException& ex) {
            const Error error = ex.get_error();
            if (error == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
                // If the output queue is full, then just poll
                producer_.poll();
            }
            else {
                throw;
            }
        }
    }
}

template <typename BufferType>
Configuration BufferedProducer<BufferType>::prepare_configuration(Configuration config) {
    using std::placeholders::_2;
    auto callback = std::bind(&BufferedProducer<BufferType>::on_delivery_report, this, _2);
    config.set_delivery_report_callback(std::move(callback));
    return config;
}

template <typename BufferType>
void BufferedProducer<BufferType>::on_delivery_report(const Message& message) {
    const IndexType index = reinterpret_cast<IndexType>(message.get_private_data());
    auto iter = messages_.find(index);
    // Got an ACK for an unexpected message?
    if (iter == messages_.end()) {
        return;
    }
    // If there was an error sending this message, then we need to re-send it
    if (message.get_error()) {
        failed_indexes_.push_back(index);
    }
    else {
        messages_.erase(iter);
    }
}

} // cppkafka

#endif // CPPKAFKA_BUFFERED_PRODUCER_H