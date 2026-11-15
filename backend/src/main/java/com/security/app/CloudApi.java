package com.security.app;

import jakarta.validation.Valid;
import jakarta.validation.constraints.*;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.transaction.annotation.Transactional;
import org.springframework.web.bind.annotation.*;
import org.springframework.web.server.ResponseStatusException;
import java.time.Clock;
import java.util.*;
import static org.springframework.http.HttpStatus.*;

@RestController
@RequestMapping("/api")
public class CloudApi {
    private final JdbcTemplate db;
    private final AuthService auth;
    private final Clock clock;
    private final long leaseMs, commandTtl;
    public CloudApi(JdbcTemplate db, AuthService auth, Clock clock,
                    @Value("${security.viewer-lease-ms}") long leaseMs,
                    @Value("${security.command-ttl-ms}") long commandTtl) {
        this.db = db; this.auth = auth; this.clock = clock; this.leaseMs = leaseMs; this.commandTtl = commandTtl;
    }
    public record Credentials(@NotBlank @Email @Size(max=200) String email, @NotNull @Size(min=12,max=72) String password) {}
    public record NewDevice(@NotBlank @Size(max=80) String name, @NotNull @Pattern(regexp="SECURITY|CLIMATE") String role) {}
    public record Command(@Min(1) @Max(4) int code) {}
    public record Event(@NotNull @Pattern(regexp="[a-f0-9]{32}") String id,
                        @NotNull @Pattern(regexp="DISTANCE|LIGHT|OVERHEAT") String kind,
                        @Positive Long occurredAt, @PositiveOrZero long uptimeMs) {}
    public record Poll(@Pattern(regexp="01[a-f0-9]{34}") String packet,
                       @PositiveOrZero long ackId,
                       @Pattern(regexp="APPLIED|REJECTED") String ackStatus,
                       @NotNull @Size(max=8) List<@Valid Event> alerts,
                       @PositiveOrZero long lostAlerts) {}

    @GetMapping("/health")
    public Map<String,String> health() { db.queryForObject("SELECT 1",Integer.class); return Map.of("status","ok"); }

    @PostMapping("/auth/register")
    public Map<String,String> register(@Valid @RequestBody Credentials c) { return Map.of("token", auth.login(c.email(), c.password(), true)); }
    @PostMapping("/auth/login")
    public Map<String,String> login(@Valid @RequestBody Credentials c) { return Map.of("token", auth.login(c.email(), c.password(), false)); }
    @PostMapping("/auth/logout")
    public void logout(@RequestHeader(value="Authorization",required=false) String token) { auth.logout(token); }

    @GetMapping("/devices")
    public List<Map<String,Object>> devices(@RequestHeader(value="Authorization",required=false) String token) {
        return db.queryForList("SELECT id,name,role,last_seen,lost_alerts FROM device WHERE owner_id=? ORDER BY name", auth.user(token));
    }
    @PostMapping("/devices")
    public Map<String,String> add(@RequestHeader(value="Authorization",required=false) String token, @Valid @RequestBody NewDevice d) {
        String user = auth.user(token), id = UUID.randomUUID().toString(), key = auth.newToken();
        db.update("INSERT INTO device(id,owner_id,name,role,token_hash) VALUES (?,?,?,?,?)", id,user,d.name().trim(),d.role(),AuthService.digest(key));
        return Map.of("id",id,"token",key);
    }
    // Re-provision an existing owned node without losing its history.
    @PostMapping("/devices/{id}/token")
    public Map<String,String> rotate(@RequestHeader(value="Authorization",required=false) String token, @PathVariable String id) {
        owned(id, token); String key = auth.newToken();
        db.update("UPDATE device SET token_hash=? WHERE id=?", AuthService.digest(key), id);
        return Map.of("id",id,"token",key);
    }
    @PostMapping("/devices/{id}/live")
    @Transactional
    public Map<String,Object> live(@RequestHeader(value="Authorization",required=false) String token, @PathVariable String id) {
        owned(id,token);
        db.update("UPDATE device SET viewer_until=? WHERE id=?", clock.millis()+leaseMs,id);
        return snapshot(id);
    }
    @GetMapping("/devices/{id}")
    public Map<String,Object> get(@RequestHeader(value="Authorization",required=false) String token, @PathVariable String id) {
        owned(id,token); return snapshot(id);
    }
    private Map<String,Object> snapshot(String id) {
        var result = new LinkedHashMap<>(db.queryForMap("SELECT id,name,role,last_seen,packet,sampled_at,lost_alerts FROM device WHERE id=?", id));
        Long seen = (Long)result.get("last_seen");
        result.put("online", seen != null && clock.millis()-seen < 15000);
        result.put("serverTime",clock.millis());
        result.put("alerts", db.queryForList("SELECT event_id,kind,occurred_at,uptime_ms,received_at FROM alert WHERE device_id=? ORDER BY received_at DESC,event_id DESC LIMIT 100",id));
        db.update("UPDATE device_command SET status='EXPIRED' WHERE device_id=? AND status='PENDING' AND expires_at<=?",id,clock.millis());
        result.put("commands", db.queryForList("SELECT id,code,status,created_at,expires_at FROM device_command WHERE device_id=? ORDER BY id DESC LIMIT 10",id));
        return result;
    }
    @PostMapping("/devices/{id}/commands")
    @Transactional
    public Map<String,Object> command(@RequestHeader(value="Authorization",required=false) String token, @PathVariable String id, @Valid @RequestBody Command c) {
        owned(id,token);
        var d = db.queryForMap("SELECT role,last_seen FROM device WHERE id=? FOR UPDATE",id);
        if (!"SECURITY".equals(d.get("role"))) throw new ResponseStatusException(CONFLICT,"Climate node has no actuators");
        Long seen = (Long)d.get("last_seen");
        if (seen == null || clock.millis()-seen >= 15000) throw new ResponseStatusException(CONFLICT,"Device offline");
        long now = clock.millis();
        if (db.queryForObject("SELECT count(*) FROM device_command WHERE device_id=? AND status='PENDING' AND expires_at>?",Integer.class,id,now) >= 10)
            throw new ResponseStatusException(TOO_MANY_REQUESTS,"Command queue full");
        db.update("INSERT INTO device_command(device_id,code,created_at,expires_at) VALUES (?,?,?,?)",id,c.code(),now,now+commandTtl);
        return db.queryForMap("SELECT id,status FROM device_command WHERE device_id=? ORDER BY id DESC LIMIT 1",id);
    }
    @PostMapping("/nodes/{id}/poll")
    @Transactional
    public Map<String,Object> poll(@RequestHeader(value="Authorization",required=false) String token, @PathVariable String id, @Valid @RequestBody Poll p) {
        auth.device(id,token);
        var d = db.queryForMap("SELECT role,viewer_until FROM device WHERE id=? FOR UPDATE",id);
        long now = clock.millis();
        boolean requested = (Long)d.get("viewer_until") > now;
        db.update("UPDATE device SET last_seen=?,lost_alerts=? WHERE id=?",now,p.lostAlerts(),id);
        if (p.packet()!=null) {
            boolean climate = (Integer.parseInt(p.packet().substring(2,4),16)&32)!=0;
            if (climate != "CLIMATE".equals(d.get("role"))) throw new ResponseStatusException(CONFLICT,"Node role mismatch");
            if (requested) db.update("UPDATE device SET packet=?,sampled_at=? WHERE id=?",p.packet(),now,id);
        }
        db.update("UPDATE device_command SET status='EXPIRED' WHERE device_id=? AND status='PENDING' AND expires_at<=?",id,now);
        if (p.ackId()>0) {
            if (p.ackStatus()==null) throw new ResponseStatusException(BAD_REQUEST,"Missing command status");
            db.update("UPDATE device_command SET status=?,completed_at=? WHERE id=? AND device_id=? AND status='PENDING'",p.ackStatus(),now,p.ackId(),id);
        }
        List<String> ackEvents = new ArrayList<>();
        for (Event e : p.alerts()) {
            if (db.queryForObject("SELECT count(*) FROM alert WHERE device_id=? AND event_id=?",Integer.class,id,e.id())==0)
                db.update("INSERT INTO alert VALUES (?,?,?,?,?,?)",id,e.id(),e.kind(),e.occurredAt(),e.uptimeMs(),now);
            ackEvents.add(e.id());
        }
        var pending = db.queryForList("SELECT id,code,expires_at FROM device_command WHERE device_id=? AND status='PENDING' ORDER BY id LIMIT 1",id);
        if (!pending.isEmpty()) pending.getFirst().put("ttlMs",(Long)pending.getFirst().remove("expires_at")-now);
        Map<String,Object> response = new LinkedHashMap<>();
        response.put("telemetry",requested);
        response.put("leaseMs",requested ? (Long)d.get("viewer_until")-now : 0);
        response.put("ackEvents",ackEvents);
        response.put("command",pending.isEmpty()?null:pending.getFirst());
        return response;
    }
    private void owned(String id, String token) {
        String user = auth.user(token);
        if (db.queryForObject("SELECT count(*) FROM device WHERE id=? AND owner_id=?",Integer.class,id,user)==0) throw new ResponseStatusException(NOT_FOUND);
    }
}
