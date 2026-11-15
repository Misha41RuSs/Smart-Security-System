package com.security.app;

import java.util.Map;
import org.springframework.http.ResponseEntity;
import org.springframework.http.converter.HttpMessageNotReadableException;
import org.springframework.web.bind.MethodArgumentNotValidException;
import org.springframework.web.bind.annotation.*;

@RestControllerAdvice
class ApiErrors {
    // Do not echo or log rejected passwords, device tokens or complete request bodies.
    @ExceptionHandler({MethodArgumentNotValidException.class,HttpMessageNotReadableException.class})
    ResponseEntity<Map<String,String>> invalidBody(Exception ignored) {
        return ResponseEntity.badRequest().body(Map.of("error","Invalid request"));
    }
}
