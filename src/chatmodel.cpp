#include "chatmodel.h"

#include <QListIterator>
#include <QByteArray>
#include <QBitArray>

ChatModel::ChatModel(TDLibWrapper *tdLibWrapper)
{
    this->tdLibWrapper = tdLibWrapper;
    this->inReload = false;
    this->inIncrementalUpdate = false;
    connect(this->tdLibWrapper, SIGNAL(messagesReceived(QVariantList)), this, SLOT(handleMessagesReceived(QVariantList)));
    connect(this->tdLibWrapper, SIGNAL(newMessageReceived(QString, QVariantMap)), this, SLOT(handleNewMessageReceived(QString, QVariantMap)));
    connect(this->tdLibWrapper, SIGNAL(chatReadInboxUpdated(QString, int)), this, SLOT(handleChatReadInboxUpdated(QString, int)));
    connect(this->tdLibWrapper, SIGNAL(chatReadOutboxUpdated(QString, QString)), this, SLOT(handleChatReadOutboxUpdated(QString, QString)));
}

ChatModel::~ChatModel()
{

}

int ChatModel::rowCount(const QModelIndex &) const
{
    return messages.size();
}

QVariant ChatModel::data(const QModelIndex &index, int role) const
{
    if(index.isValid() && role == Qt::DisplayRole) {
        return QVariant(messages.value(index.row()));
    }
    return QVariant();
}

bool ChatModel::insertRows(int row, int count, const QModelIndex &parent)
{
    qDebug() << "[ChatModel] Inserting at " << row << ", row count: " << count;
    beginInsertRows(parent, row, row + count - 1);
    for (int i = 0; i < count; i++) {
        this->messages.insert(row + i, this->messagesToBeAdded.at(i));
    }
    this->calculateMessageIndexMap();
    endInsertRows();
    return true;
}

void ChatModel::initialize(const QVariantMap &chatInformation)
{
    qDebug() << "[ChatModel] Initializing chat model...";
    this->chatInformation = chatInformation;
    this->messages.clear();
    this->messageIndexMap.clear();
    this->messagesToBeAdded.clear();
    this->chatId = chatInformation.value("id").toString();
    tdLibWrapper->getChatHistory(this->chatId);
}

void ChatModel::triggerLoadMoreHistory()
{
    if (!this->inIncrementalUpdate) {
        qDebug() << "[ChatModel] Trigger loading older history...";
        this->inIncrementalUpdate = true;
        this->tdLibWrapper->getChatHistory(this->chatId, this->messages.first().toMap().value("id").toLongLong());
    }
}

bool compareMessages(const QVariant &message1, const QVariant &message2)
{
    QVariantMap messageMap1 = message1.toMap();
    QVariantMap messageMap2 = message2.toMap();
    if (messageMap1.value("id").toLongLong() < messageMap2.value("id").toLongLong()) {
        return true;
    } else {
        return false;
    }
}

void ChatModel::handleMessagesReceived(const QVariantList &messages)
{
    qDebug() << "[ChatModel] Receiving new messages :)" << messages.size();

    if (messages.size() == 0) {
        qDebug() << "[ChatModel] No additional messages loaded, notifying chat UI...";
        this->inReload = false;
        int listInboxPosition = this->calculateLastKnownMessageId();
        int listOutboxPosition = this->calculateLastReadSentMessageId();
        if (this->inIncrementalUpdate) {
            this->inIncrementalUpdate = false;
            emit messagesIncrementalUpdate(listInboxPosition, listOutboxPosition);
        } else {
            emit messagesReceived(listInboxPosition, listOutboxPosition);
        }
    } else {
        this->messagesMutex.lock();
        this->messagesToBeAdded.clear();
        QListIterator<QVariant> messagesIterator(messages);
        while (messagesIterator.hasNext()) {
            QVariantMap currentMessage = messagesIterator.next().toMap();
            if (currentMessage.value("chat_id").toString() == this->chatId) {
                this->messagesToBeAdded.append(currentMessage);
            }
        }
        std::sort(this->messagesToBeAdded.begin(), this->messagesToBeAdded.end(), compareMessages);

        this->insertMessages();
        this->messagesMutex.unlock();

        // First call only returns a few messages, we need to get a little more than that...
        if (this->messagesToBeAdded.size() < 10 && !this->inReload) {
            qDebug() << "[ChatModel] Only a few messages received in first call, loading more...";
            this->inReload = true;
            this->tdLibWrapper->getChatHistory(this->chatId, this->messagesToBeAdded.first().toMap().value("id").toLongLong());
        } else {
            qDebug() << "[ChatModel] Messages loaded, notifying chat UI...";
            this->inReload = false;
            int listInboxPosition = this->calculateLastKnownMessageId();
            int listOutboxPosition = this->calculateLastReadSentMessageId();
            if (this->inIncrementalUpdate) {
                this->inIncrementalUpdate = false;
                emit messagesIncrementalUpdate(listInboxPosition, listOutboxPosition);
            } else {
                emit messagesReceived(listInboxPosition, listOutboxPosition);
            }
        }
    }

}

void ChatModel::handleNewMessageReceived(const QString &chatId, const QVariantMap &message)
{
    if (chatId == this->chatId) {
        qDebug() << "[ChatModel] New message received for this chat";
        this->messagesMutex.lock();

        this->messagesToBeAdded.clear();
        this->messagesToBeAdded.append(message);

        this->insertMessages();
        this->messagesMutex.unlock();
        emit newMessageReceived();
    }
}

void ChatModel::handleChatReadInboxUpdated(const QString &chatId, const int &unreadCount)
{
    if (chatId == this->chatId) {
        qDebug() << "[ChatModel] Updating chat unread count, unread messages " << unreadCount;
        this->chatInformation.insert("unread_count", unreadCount);
        emit unreadCountUpdated(unreadCount);
    }
}

void ChatModel::handleChatReadOutboxUpdated(const QString &chatId, const QString &lastReadOutboxMessageId)
{
    if (chatId == this->chatId) {
        this->chatInformation.insert("last_read_outbox_message_id", lastReadOutboxMessageId);
        int sentIndex = calculateLastReadSentMessageId();
        qDebug() << "[ChatModel] Updating sent message ID, new index " << sentIndex;
        emit lastReadSentMessageUpdated(sentIndex);
    }
}

void ChatModel::insertMessages()
{
    if (this->messages.isEmpty()) {
        beginResetModel();
        this->messages.append(this->messagesToBeAdded);
        this->calculateMessageIndexMap();
        endResetModel();
    } else {
        // There is only an append or a prepend, tertium non datur! (probably ;))
        if (this->messages.last().toMap().value("id").toLongLong() < this->messagesToBeAdded.first().toMap().value("id").toLongLong()) {
            // Append
            this->insertRows(rowCount(QModelIndex()), this->messagesToBeAdded.size());
        } else {
            // Prepend
            this->insertRows(0, this->messagesToBeAdded.size());
        }
    }
}

QVariantMap ChatModel::enhanceMessage(const QVariantMap &message)
{
    QVariantMap enhancedMessage = message;
    if (enhancedMessage.value("content").toMap().value("@type").toString() == "messageVoiceNote" ) {
        QVariantMap contentMap = enhancedMessage.value("content").toMap();
        QVariantMap voiceNoteMap = contentMap.value("voice_note").toMap();
        QByteArray waveBytes = QByteArray::fromBase64(voiceNoteMap.value("waveform").toByteArray());
        QBitArray waveBits(waveBytes.count() * 8);

        for (int i = 0; i < waveBytes.count(); i++) {
            for (int b = 0; b < 8; b++) {
                waveBits.setBit( i * 8 + b, waveBytes.at(i) & (1 << (7 - b)) );
            }
        }
        int waveSize = 10;
        int waveformSets = waveBits.size() / waveSize;
        QVariantList decodedWaveform;
        for (int i = 0; i < waveformSets; i++) {
            int waveformHeight = 0;
            for (int j = 0; j < waveSize; j++) {
                waveformHeight = waveformHeight + ( waveBits.at(i * waveSize + j) * (2 ^ (j)) );
            }
            decodedWaveform.append(waveformHeight);
        }
        voiceNoteMap.insert("decoded_voice_note", decodedWaveform);
        contentMap.insert("voice_note", voiceNoteMap);
        enhancedMessage.insert("content", contentMap);
    }
    return enhancedMessage;
}

int ChatModel::calculateLastKnownMessageId()
{
    qDebug() << "[ChatModel] calculateLastKnownMessageId";
    QString lastKnownMessageId = this->chatInformation.value("last_read_inbox_message_id").toString();
    qDebug() << "[ChatModel] lastKnownMessageId" << lastKnownMessageId;
    qDebug() << "[ChatModel] size messageIndexMap" << this->messageIndexMap.size();
    qDebug() << "[ChatModel] contains ID?" << this->messageIndexMap.contains(lastKnownMessageId);
    int listInboxPosition = this->messageIndexMap.value(lastKnownMessageId, this->messages.size() - 1).toInt();
    qDebug() << "[ChatModel] Last known message is at position" << listInboxPosition;
    return listInboxPosition;
}

int ChatModel::calculateLastReadSentMessageId()
{
    qDebug() << "[ChatModel] calculateLastReadSentMessageId";
    QString lastReadSentMessageId = this->chatInformation.value("last_read_outbox_message_id").toString();
    qDebug() << "[ChatModel] lastReadSentMessageId" << lastReadSentMessageId;
    qDebug() << "[ChatModel] size messageIndexMap" << this->messageIndexMap.size();
    qDebug() << "[ChatModel] contains ID?" << this->messageIndexMap.contains(lastReadSentMessageId);
    int listOutboxPosition = this->messageIndexMap.value(lastReadSentMessageId, this->messages.size() - 1).toInt();
    qDebug() << "[ChatModel] Last read sent message is at position" << listOutboxPosition;
    return listOutboxPosition;
}

void ChatModel::calculateMessageIndexMap()
{
    qDebug() << "[ChatModel] calculateMessageIndexMap";
    this->messageIndexMap.clear();
    for (int i = 0; i < this->messages.size(); i++) {
        this->messageIndexMap.insert(this->messages.at(i).toMap().value("id").toString(), i);
    }
}