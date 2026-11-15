package com.security.app;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.dao.DuplicateKeyException;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.security.crypto.bcrypt.BCryptPasswordEncoder;
import org.springframework.stereotype.Service;
import org.springframework.web.server.ResponseStatusException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.time.Clock;
import java.util.HexFormat;
import java.util.Locale;
import java.util.UUID;
import static org.springframework.http.HttpStatus.*;

@Service
public class AuthService {
    private final JdbcTemplate db;
    private final Clock clock;
    private final boolean registration;
    private final long sessionHours;
    private final BCryptPasswordEncoder passwords = new BCryptPasswordEncoder(12);
    private final String dummyHash = passwords.encode("not-a-real-user-password");
    private final SecureRandom random = new SecureRandom();
    AuthService(JdbcTemplate db, Clock clock, @Value("${security.registration-enabled}") boolean registration,
                @Value("${security.session-hours}") long sessionHours) {
        this.db = db; this.clock = clock; this.registration = registration; this.sessionHours = sessionHours;
    }
    public String login(String email, String password, boolean create) {
        email = email.trim().toLowerCase(Locale.ROOT);
        if (password.getBytes(StandardCharsets.UTF_8).length > 72) throw new ResponseStatusException(BAD_REQUEST, "Password exceeds 72 UTF-8 bytes");
        if (create) {
            if (!registration) throw new ResponseStatusException(FORBIDDEN, "Registration disabled");
            try { db.update("INSERT INTO app_user VALUES (?,?,?)", UUID.randomUUID().toString(), email, passwords.encode(password)); }
            catch (DuplicateKeyException e) { throw new ResponseStatusException(CONFLICT, "Account already exists"); }
        }
        var users = db.queryForList("SELECT id,password_hash FROM app_user WHERE email=?", email);
        String hash = users.isEmpty() ? dummyHash : (String) users.getFirst().get("password_hash");
        if (!passwords.matches(password, hash) || users.isEmpty()) throw new ResponseStatusException(UNAUTHORIZED, "Invalid credentials");
        String token = newToken();
        db.update("DELETE FROM user_session WHERE expires_at<?", clock.millis());
        db.update("INSERT INTO user_session VALUES (?,?,?)", digest(token), users.getFirst().get("id"), clock.millis() + sessionHours * 3600000);
        return token;
    }
    public String user(String authorization) {
        var rows = db.queryForList("SELECT user_id FROM user_session WHERE token_hash=? AND expires_at>?", digest(bearer(authorization)), clock.millis());
        if (rows.isEmpty()) throw new ResponseStatusException(UNAUTHORIZED);
        return (String) rows.getFirst().get("user_id");
    }
    public void logout(String authorization) {
        db.update("DELETE FROM user_session WHERE token_hash=?", digest(bearer(authorization)));
    }
    public void device(String id, String authorization) {
        var rows = db.queryForList("SELECT token_hash FROM device WHERE id=?", id);
        String candidate = digest(bearer(authorization));
        if (rows.isEmpty() || !MessageDigest.isEqual(candidate.getBytes(StandardCharsets.US_ASCII), ((String)rows.getFirst().get("token_hash")).getBytes(StandardCharsets.US_ASCII)))
            throw new ResponseStatusException(UNAUTHORIZED);
    }
    private String bearer(String value) {
        if (value == null || !value.matches("Bearer [a-f0-9]{64}")) throw new ResponseStatusException(UNAUTHORIZED);
        return value.substring(7);
    }
    public String newToken() { byte[] bytes = new byte[32]; random.nextBytes(bytes); return HexFormat.of().formatHex(bytes); }
    public static String digest(String value) {
        try { return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(value.getBytes(StandardCharsets.UTF_8))); }
        catch (java.security.NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }
}
