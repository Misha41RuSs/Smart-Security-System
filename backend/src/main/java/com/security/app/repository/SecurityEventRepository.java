package com.security.app.repository;

import com.security.app.entity.SecurityEvent;
import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.stereotype.Repository;

import java.time.LocalDateTime;
import java.util.List;

@Repository
public interface SecurityEventRepository extends JpaRepository<SecurityEvent, Long> {
    List<SecurityEvent> findTop5ByOrderByTimestampDesc();
    long countByTimestampAfter(LocalDateTime timestamp);
}
