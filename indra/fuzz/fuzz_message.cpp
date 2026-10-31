/**
 * @file fuzz_message.cpp
 * @brief libFuzzer harness: the UDP message system's receive path on a raw datagram.
 *
 * Replays what LLMessageSystem::checkMessages does after the socket read:
 *   ack-count trim -> zeroCodeExpand -> LLTemplateMessageReader::validateMessage -> readMessage
 * using the real scripts/messages/message_template.msg. Handlers are not registered, so this
 * covers the wire decode (template lookup, block / variable bounds, variable-length fields,
 * zero-code expansion), not what the viewer does with a decoded message.
 *
 * Input layout: byte 0 bit 0 = "circuit is trusted", the rest is the datagram as received
 * (byte 0 flags, 1-4 sequence number, 5 extra-header length, then the message number).
 *
 * Not covered: circuit lookup, duplicate-resend suppression, the LLPacketRing receive itself.
 *
 * Setup note: a real LLMessageSystem is constructed (it binds one ephemeral UDP socket on
 * port 0) because the reader dereferences gMessageSystem on the "ran off end of packet" and
 * handler-timing paths, and because zeroCodeExpand() is a member of it. The reader under test
 * is our own, over a map built here, since the system's reader and number map are private.
 */
#include "fuzz_common.h"

#include "llmessagetemplate.h"
#include "llmessagetemplateparser.h"
#include "lltemplatemessagereader.h"
#include "message.h"

#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

#ifndef FUZZ_MESSAGE_TEMPLATE
#define FUZZ_MESSAGE_TEMPLATE "scripts/messages/message_template.msg"
#endif

namespace
{
LLMessageSystem* gSystem = nullptr;
LLTemplateMessageReader::message_template_number_map_t gNumbers;
LLTemplateMessageReader* gReader = nullptr;
U8 gBuffer[MAX_BUFFER_SIZE];
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();

    const char* env = std::getenv("FUZZ_MESSAGE_TEMPLATE");
    const std::string path = (env && *env) ? env : FUZZ_MESSAGE_TEMPLATE;

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "fuzz_message: cannot open message template %s (set FUZZ_MESSAGE_TEMPLATE)\n",
                     path.c_str());
        std::exit(1);
    }
    std::stringstream body;
    body << file.rdbuf();

    // port 0: any free port; failure_is_fatal false so a bad template reports instead of LL_ERRS
    gSystem = new LLMessageSystem(path, 0, 1, 0, 0, false, 5.f, 100.f);
    if (!gSystem->isOK())
    {
        std::fprintf(stderr, "fuzz_message: LLMessageSystem failed to start (socket?)\n");
        std::exit(1);
    }
    gMessageSystem = gSystem;

    LLTemplateTokenizer tokens(body.str());
    LLTemplateParser parsed(tokens);
    for (auto it = parsed.getMessagesBegin(); it != parsed.getMessagesEnd(); ++it)
    {
        gNumbers[(*it)->mMessageNumber] = *it;   // same as LLMessageSystem::addTemplate
    }
    gReader = new LLTemplateMessageReader(gNumbers);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1 + static_cast<size_t>(LL_MINIMUM_VALID_PACKET_SIZE))
    {
        return 0;
    }
    const bool trusted = (data[0] & 1) != 0;
    size_t length = size - 1;
    if (length > static_cast<size_t>(MAX_BUFFER_SIZE))
    {
        length = MAX_BUFFER_SIZE;   // the socket buffer is this big; nothing larger is ever read
    }
    std::memcpy(gBuffer, data + 1, length);

    U8* buffer = gBuffer;
    S32 receive_size = static_cast<S32>(length);

    // appended packet acks (checkMessages)
    if (buffer[0] & LL_ACK_FLAG)
    {
        const S32 acks = buffer[--receive_size];
        if (receive_size >= static_cast<S32>(acks * sizeof(TPACKETID) + LL_MINIMUM_VALID_PACKET_SIZE))
        {
            receive_size -= acks * sizeof(TPACKETID);
        }
        else
        {
            return 0;   // malformed: the viewer drops it
        }
    }

    gSystem->zeroCodeExpand(&buffer, &receive_size);
    if (receive_size < LL_MINIMUM_VALID_PACKET_SIZE)
    {
        return 0;
    }

    const LLHost sender(0x7f000001, 13000);
    if (gReader->validateMessage(buffer, receive_size, sender, trusted))
    {
        gReader->readMessage(buffer, sender);
    }
    gReader->clearMessage();
    return 0;
}
