package com.security.app.repository;

import com.security.app.entity.CommandLog;
import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.stereotype.Repository;

@Repository
public interface CommandLogRepository extends JpaRepository<CommandLog, Long> {
}
