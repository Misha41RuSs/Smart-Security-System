package com.security.app;

import com.security.app.entity.SystemSetting;
import com.security.app.repository.SubscriberRepository;
import com.security.app.repository.SystemSettingRepository;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;

import java.util.ArrayList;
import java.util.Optional;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.*;

public class AppStateTest {

    private AppState appState;

    @Mock
    private SystemSettingRepository systemSettingRepository;

    @Mock
    private SubscriberRepository subscriberRepository;

    @BeforeEach
    public void setUp() {
        MockitoAnnotations.openMocks(this);
        appState = new AppState(systemSettingRepository, subscriberRepository);
    }

    @Test
    public void testInitWithDefaultSettings() {
        when(systemSettingRepository.findById("distance_threshold")).thenReturn(Optional.empty());
        when(systemSettingRepository.findById("light_threshold")).thenReturn(Optional.empty());
        when(subscriberRepository.findByActiveTrue()).thenReturn(new ArrayList<>());

        appState.init();

        assertEquals(5.0, appState.getDistanceThreshold());
        assertEquals(800, appState.getLightThreshold());

        // Проверяем, что настройки по умолчанию сохранились в БД
        verify(systemSettingRepository, times(2)).save(any(SystemSetting.class));
    }

    @Test
    public void testInitWithDbSettings() {
        SystemSetting distSetting = new SystemSetting("distance_threshold", "10.5");
        SystemSetting lightSetting = new SystemSetting("light_threshold", "500");

        when(systemSettingRepository.findById("distance_threshold")).thenReturn(Optional.of(distSetting));
        when(systemSettingRepository.findById("light_threshold")).thenReturn(Optional.of(lightSetting));
        when(subscriberRepository.findByActiveTrue()).thenReturn(new ArrayList<>());

        appState.init();

        assertEquals(10.5, appState.getDistanceThreshold());
        assertEquals(500, appState.getLightThreshold());
    }

    @Test
    public void testSetDistanceThreshold() {
        appState.setDistanceThreshold(12.0);
        assertEquals(12.0, appState.getDistanceThreshold());
        verify(systemSettingRepository, times(1)).save(any(SystemSetting.class));
    }
}
