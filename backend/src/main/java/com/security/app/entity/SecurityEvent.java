package com.security.app.entity;

import jakarta.persistence.*;

import java.time.LocalDateTime;

@Entity
@Table(name = "security_events")
/**
 * Entity class representing a security alert or event.
 */
public class SecurityEvent {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    @Column(name = "timestamp")
    private LocalDateTime timestamp;

    @Column(name = "event_type")
    private String eventType; // e.g., DISTANCE, LIGHT

    @Column(name = "value")
    private Double value;

    @Column(name = "message")
    private String message;

    public SecurityEvent() {
    }

    public SecurityEvent(Long id, LocalDateTime timestamp, String eventType, Double value, String message) {
        this.id = id;
        this.timestamp = timestamp;
        this.eventType = eventType;
        this.value = value;
        this.message = message;
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

    public String getEventType() {
        return eventType;
    }

    public void setEventType(String eventType) {
        this.eventType = eventType;
    }

    public Double getValue() {
        return value;
    }

    public void setValue(Double value) {
        this.value = value;
    }

    public String getMessage() {
        return message;
    }

    public void setMessage(String message) {
        this.message = message;
    }

    public static Builder builder() {
        return new Builder();
    }

    public static class Builder {
        private Long id;
        private LocalDateTime timestamp;
        private String eventType;
        private Double value;
        private String message;

        public Builder id(Long id) {
            this.id = id;
            return this;
        }

        public Builder timestamp(LocalDateTime timestamp) {
            this.timestamp = timestamp;
            return this;
        }

        public Builder eventType(String eventType) {
            this.eventType = eventType;
            return this;
        }

        public Builder value(Double value) {
            this.value = value;
            return this;
        }

        public Builder message(String message) {
            this.message = message;
            return this;
        }

        public SecurityEvent build() {
            return new SecurityEvent(id, timestamp, eventType, value, message);
        }
    }
}
