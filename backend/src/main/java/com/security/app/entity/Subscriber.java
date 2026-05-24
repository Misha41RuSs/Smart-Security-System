package com.security.app.entity;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.Id;
import jakarta.persistence.Table;

import java.time.LocalDateTime;

@Entity
@Table(name = "subscribers")
public class Subscriber {

    @Id
    @Column(name = "chat_id")
    private Long chatId;

    @Column(name = "username")
    private String username;

    @Column(name = "first_name")
    private String firstName;

    @Column(name = "last_name")
    private String lastName;

    @Column(name = "subscribed_at")
    private LocalDateTime subscribedAt;

    @Column(name = "active")
    private boolean active;

    public Subscriber() {
    }

    public Subscriber(Long chatId, String username, String firstName, String lastName, LocalDateTime subscribedAt, boolean active) {
        this.chatId = chatId;
        this.username = username;
        this.firstName = firstName;
        this.lastName = lastName;
        this.subscribedAt = subscribedAt;
        this.active = active;
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

    public String getFirstName() {
        return firstName;
    }

    public void setFirstName(String firstName) {
        this.firstName = firstName;
    }

    public String getLastName() {
        return lastName;
    }

    public void setLastName(String lastName) {
        this.lastName = lastName;
    }

    public LocalDateTime getSubscribedAt() {
        return subscribedAt;
    }

    public void setSubscribedAt(LocalDateTime subscribedAt) {
        this.subscribedAt = subscribedAt;
    }

    public boolean isActive() {
        return active;
    }

    public void setActive(boolean active) {
        this.active = active;
    }

    public static Builder builder() {
        return new Builder();
    }

    public static class Builder {
        private Long chatId;
        private String username;
        private String firstName;
        private String lastName;
        private LocalDateTime subscribedAt;
        private boolean active;

        public Builder chatId(Long chatId) {
            this.chatId = chatId;
            return this;
        }

        public Builder username(String username) {
            this.username = username;
            return this;
        }

        public Builder firstName(String firstName) {
            this.firstName = firstName;
            return this;
        }

        public Builder lastName(String lastName) {
            this.lastName = lastName;
            return this;
        }

        public Builder subscribedAt(LocalDateTime subscribedAt) {
            this.subscribedAt = subscribedAt;
            return this;
        }

        public Builder active(boolean active) {
            this.active = active;
            return this;
        }

        public Subscriber build() {
            return new Subscriber(chatId, username, firstName, lastName, subscribedAt, active);
        }
    }
}
