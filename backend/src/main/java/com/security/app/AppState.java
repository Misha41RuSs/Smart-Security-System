package com.security.app;

import com.security.app.entity.Subscriber;
import com.security.app.entity.SystemSetting;
import com.security.app.repository.SubscriberRepository;
import com.security.app.repository.SystemSettingRepository;
import jakarta.annotation.PostConstruct;
import org.springframework.stereotype.Component;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.stream.Collectors;

@Component
public class AppState {
    private final SystemSettingRepository systemSettingRepository;
    private final SubscriberRepository subscriberRepository;

    private double distanceThreshold = 5.0; // cm
    private int lightThreshold = 800;
    private final Set<Long> subscribedChatIds = ConcurrentHashMap.newKeySet();

    public AppState(SystemSettingRepository systemSettingRepository, SubscriberRepository subscriberRepository) {
        this.systemSettingRepository = systemSettingRepository;
        this.subscriberRepository = subscriberRepository;
    }

    @PostConstruct
    public void init() {
        try {
            // Загрузка порогов из БД
            systemSettingRepository.findById("distance_threshold").ifPresentOrElse(
                setting -> this.distanceThreshold = Double.parseDouble(setting.getValue()),
                () -> saveSetting("distance_threshold", String.valueOf(distanceThreshold))
            );

            systemSettingRepository.findById("light_threshold").ifPresentOrElse(
                setting -> this.lightThreshold = Integer.parseInt(setting.getValue()),
                () -> saveSetting("light_threshold", String.valueOf(lightThreshold))
            );

            // Загрузка активных подписчиков из БД
            subscribedChatIds.addAll(
                subscriberRepository.findByActiveTrue().stream()
                    .map(Subscriber::getChatId)
                    .collect(Collectors.toList())
            );
            
            System.out.println("====== APP STATE INITIALIZED FROM DATABASE ======");
            System.out.println("Distance Threshold: " + distanceThreshold + " cm");
            System.out.println("Light Threshold: " + lightThreshold);
            System.out.println("Subscribers loaded: " + subscribedChatIds.size());
        } catch (Exception e) {
            System.err.println("Error initializing AppState from DB. Make sure database is running.");
            e.printStackTrace();
        }
    }

    public double getDistanceThreshold() {
        return distanceThreshold;
    }

    public void setDistanceThreshold(double distanceThreshold) {
        this.distanceThreshold = distanceThreshold;
        saveSetting("distance_threshold", String.valueOf(distanceThreshold));
    }

    public int getLightThreshold() {
        return lightThreshold;
    }

    public void setLightThreshold(int lightThreshold) {
        this.lightThreshold = lightThreshold;
        saveSetting("light_threshold", String.valueOf(lightThreshold));
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

    private void saveSetting(String key, String value) {
        try {
            SystemSetting setting = SystemSetting.builder()
                .key(key)
                .value(value)
                .build();
            systemSettingRepository.save(setting);
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
