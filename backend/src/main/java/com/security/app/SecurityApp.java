package com.security.app;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import java.time.Clock;
@SpringBootApplication
public class SecurityApp {
    public static void main(String[] args) { SpringApplication.run(SecurityApp.class, args); }
    @Bean Clock clock() { return Clock.systemUTC(); }
}
