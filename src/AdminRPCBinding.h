/*
 * This file is part of the bitcoin-classic project
 * Copyright (C) 2016-2017 Tom Zander <tomz@freedommail.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ADMINRPCBINDING_H
#define ADMINRPCBINDING_H

#include <string>
#include <functional>

namespace Streaming {
    class MessageBuilder;
}
class UniValue;
class Message;

namespace AdminRPCBinding
{
    /**
     * This is a baseclass for a specific command.
     * In the admin API we have specific incoming messages which map to a Parser implementation.
     * When a new request comes in from the network, the specific parser that can handle this
     * is instantiated and then createReqeust() is called with the message we received from the network,
     * followed by a call to parser() which is meant to build the answer.
     */
    class Parser {
    public:
        /**
         * Constructor, meant for overloading. Subclasses should pass in the parameters.
         * @param method  this parameter is the name of the RPC method we map to.
         * @param replyMessageId the enum value of the answer message that the network client expects.
         * @param messageSize when passed in, this will be the hardcoded size and calculateMessageSize() will not be called.
         */
        Parser(const std::string &method, int replyMessageId, int messageSize = -1);
        virtual ~Parser() {}

        /**
         * @brief messageSize returns the amount of bytes we should reserve for the reply.
         * @param result is the result we received from the RPC layer.
         * @return either the messageSize passed into the constructor, or the output of calculateMessageSize()
         */
        inline int messageSize(const UniValue &result) const {
            if (m_messageSize > 0)
                return m_messageSize;
            return calculateMessageSize(result);
        }
        /// Returns the name of the RPC method we map
        inline const std::string &method() const {
            return m_method;
        }
        /// Returns the message-id we set in the answer message. Typically an enum from the AdminProtocol.h
        int replyMessageId() const {
            return m_replyMessageId;
        }

        /**
         * @brief createRequest takes the incoming \a message and creates a univalue request to pass to the RPC layer.
         * @param message the incoming message.
         * @param output the data that the RPC method() we map to will understand.
         */
        virtual void createRequest(const Message &message, UniValue &output);
        /**
         * @brief The buildReply method takes the results from the RPC method we map to and builds the reply to be sent over the network.
         */
        virtual void buildReply(Streaming::MessageBuilder &builder, const UniValue &result);
        /// Return the size we shall reserve for the message to be created in buildReply.
        /// This size CAN NOT be smaller than what is actually consumed in buildReply.
        virtual int calculateMessageSize(const UniValue&) const;

    protected:
        int m_messageSize;
        int m_replyMessageId;
        std::string m_method;
    };

    /// maps an input message to a Parser implementation.
    Parser* createParser(const Message &message);
}

#endif
