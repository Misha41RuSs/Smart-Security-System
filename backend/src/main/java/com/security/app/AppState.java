package com.security.app;

import org.springframework.stereotype.Component;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

@Component
public class AppState {
    private double distanceThreshold = 5.0; // cm
    private int lightThreshold = 800;
    private Set<Long> subscribedChatIds = ConcurrentHashMap.newKeySet();

    public double getDistanceThreshold() {
        return distanceThreshold;
    }

    public void setDistanceThreshold(double distanceThreshold) {
        this.distanceThreshold = distanceThreshold;
    }

    public int getLightThreshold() {
        return lightThreshold;
    }

    public void setLightThreshold(int lightThreshold) {
        this.lightThreshold = lightThreshold;
    }

    public void addChatId(Long chatId) {
        subscribedChatIds.add(chatId);
    }

    public void removeChatId(Long chatId) {
        subscribedChatIds.remove(chatId);
    }

    public Set<Long> getSubscribedChatIds() {
        return subscribedChatIds;
    }
}
