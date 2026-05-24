package com.security.app.entity;

import jakarta.persistence.*;

import java.time.LocalDateTime;

@Entity
@Table(name = "command_logs")
public class CommandLog {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    @Column(name = "timestamp")
    private LocalDateTime timestamp;

    @Column(name = "chat_id")
    private Long chatId;

    @Column(name = "username")
    private String username;

    @Column(name = "command")
    private String command;

    @Column(name = "success")
    private boolean success;

    public CommandLog() {
    }

    public CommandLog(Long id, LocalDateTime timestamp, Long chatId, String username, String command, boolean success) {
        this.id = id;
        this.timestamp = timestamp;
        this.chatId = chatId;
        this.username = username;
        this.command = command;
        this.success = success;
    }

    public Long getId() {
        return id;
    }

    public void setId(Long id) {
        this.id = id;
    }

    public LocalDateTime getTimestamp() {
        return timestamp;
    }

    public void setTimestamp(LocalDateTime timestamp) {
        this.timestamp = timestamp;
    }

    public Long getChatId() {
        return chatId;
    }

    public void setChatId(Long chatId) {
        this.chatId = chatId;
    }

    public String getUsername() {
        return username;
    }

    public void setUsername(String username) {
        this.username = username;
    }

    public String getCommand() {
        return command;
    }

    public void setCommand(String command) {
        this.command = command;
    }

    public boolean isSuccess() {
        return success;
    }

    public void setSuccess(boolean success) {
        this.success = success;
    }

    public static Builder builder() {
        return new Builder();
    }

    public static class Builder {
        private Long id;
        private LocalDateTime timestamp;
        private Long chatId;
        private String username;
        private String command;
        private boolean success;

        public Builder id(Long id) {
            this.id = id;
            return this;
        }

        public Builder timestamp(LocalDateTime timestamp) {
            this.timestamp = timestamp;
            return this;
        }

        public Builder chatId(Long chatId) {
            this.chatId = chatId;
            return this;
        }

        public Builder username(String username) {
            this.username = username;
            return this;
        }

        public Builder command(String command) {
            this.command = command;
            return this;
        }

        public Builder success(boolean success) {
            this.success = success;
            return this;
        }

        public CommandLog build() {
            return new CommandLog(id, timestamp, chatId, username, command, success);
        }
    }
}
