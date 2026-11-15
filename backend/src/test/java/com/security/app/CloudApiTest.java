package com.security.app;

import com.fasterxml.jackson.databind.*;
import org.junit.jupiter.api.*;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.*;
import org.springframework.context.annotation.*;
import org.springframework.test.web.servlet.*;
import java.time.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import static org.junit.jupiter.api.Assertions.*;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.*;

@SpringBootTest
@AutoConfigureMockMvc
@Import(CloudApiTest.TimeConfig.class)
class CloudApiTest {
    static class TestClock extends Clock {
        final AtomicLong now=new AtomicLong(1800000000000L);
        public ZoneId getZone(){return ZoneOffset.UTC;}
        public Clock withZone(ZoneId zone){return this;}
        public Instant instant(){return Instant.ofEpochMilli(now.get());}
    }
    @TestConfiguration static class TimeConfig {
        @Bean @Primary TestClock testClock(){return new TestClock();}
    }
    @Autowired MockMvc mvc;
    @Autowired ObjectMapper json;
    @Autowired TestClock clock;
    String user, node, key;
    final String packet="0103000000000a010001dc26000000000000";
    @BeforeEach void setup() throws Exception {
        user=postJson("/api/auth/register","",Map.of("email",UUID.randomUUID()+"@test.example","password","very-long-test-password"),200).get("token").asText();
        JsonNode d=postJson("/api/devices",user,Map.of("name","Guard","role","SECURITY"),200);
        node=d.get("id").asText(); key=d.get("token").asText();
    }
    JsonNode postJson(String path,String token,Object body,int status) throws Exception {
        var request=post(path).contentType("application/json").content(json.writeValueAsBytes(body));
        if(!token.isEmpty()) request.header("Authorization","Bearer "+token);
        var response=mvc.perform(request).andReturn().getResponse();
        assertEquals(status,response.getStatus(),response.getContentAsString());
        return response.getContentAsByteArray().length==0?json.nullNode():json.readTree(response.getContentAsByteArray());
    }
    Map<String,Object> pollBody(){return new HashMap<>(Map.of("alerts",List.of(),"ackId",0,"lostAlerts",0));}
    JsonNode poll(Map<String,Object> body) throws Exception{return postJson("/api/nodes/"+node+"/poll",key,body,200);}
    JsonNode live() throws Exception{return postJson("/api/devices/"+node+"/live",user,Map.of(),200);}

    @Test void telemetryRequiresViewerAndLeaseExpires() throws Exception {
        assertFalse(poll(pollBody()).get("telemetry").asBoolean());
        live();
        assertTrue(poll(pollBody()).get("telemetry").asBoolean());
        var body=pollBody(); body.put("packet",packet); poll(body);
        assertEquals(packet,live().get("packet").asText());
        clock.now.addAndGet(15001);
        assertFalse(poll(pollBody()).get("telemetry").asBoolean());
        body.put("packet","0101000000000a010001dc26000000000000"); poll(body);
        assertEquals(packet,live().get("packet").asText()); // unsolicited packet was ignored
    }
    @Test void alertsDeliveredWithoutViewerAndDeduplicatedOnRetry() throws Exception {
        var body=pollBody();
        body.put("alerts",List.of(Map.of("id","a".repeat(32),"kind","DISTANCE","uptimeMs",1234)));
        assertEquals(1,poll(body).get("ackEvents").size());
        assertEquals(1,poll(body).get("ackEvents").size());
        JsonNode state=live();
        assertEquals(1,state.get("alerts").size());
        assertTrue(state.get("alerts").get(0).get("occurred_at").isNull());
    }
    @Test void ownershipAndDeviceTokensCannotBeInterchanged() throws Exception {
        String other=postJson("/api/auth/register","",Map.of("email",UUID.randomUUID()+"@test.example","password","another-long-password"),200).get("token").asText();
        postJson("/api/devices/"+node+"/live",other,Map.of(),404);
        postJson("/api/nodes/"+node+"/poll",user,pollBody(),401);
        postJson("/api/devices/"+node+"/live",key,Map.of(),401);
        postJson("/api/nodes/"+node+"/poll","",pollBody(),401);
    }
    @Test void commandIsRetriedUntilAckAndExpiredCommandsAreNotDelivered() throws Exception {
        poll(pollBody());
        long command=postJson("/api/devices/"+node+"/commands",user,Map.of("code",2),200).get("id").asLong();
        assertEquals(command,poll(pollBody()).get("command").get("id").asLong());
        assertEquals(command,poll(pollBody()).get("command").get("id").asLong());
        var ack=pollBody(); ack.put("ackId",command);ack.put("ackStatus","APPLIED");
        assertTrue(poll(ack).get("command").isNull());
        assertEquals("APPLIED",live().get("commands").get(0).get("status").asText());
        postJson("/api/devices/"+node+"/commands",user,Map.of("code",4),200);
        clock.now.addAndGet(30001);
        assertTrue(poll(pollBody()).get("command").isNull());
        assertEquals("EXPIRED",live().get("commands").get(0).get("status").asText());
    }
    @Test void rejectsInvalidPacketsOfflineCommandsAndClimateActuators() throws Exception {
        postJson("/api/devices/"+node+"/commands",user,Map.of("code",2),409);
        var body=pollBody();body.put("packet","broken");
        postJson("/api/nodes/"+node+"/poll",key,body,400);
        JsonNode d=postJson("/api/devices",user,Map.of("name","Climate","role","CLIMATE"),200);
        node=d.get("id").asText();key=d.get("token").asText();poll(pollBody());
        postJson("/api/devices/"+node+"/commands",user,Map.of("code",2),409);
        body.put("packet",packet);postJson("/api/nodes/"+node+"/poll",key,body,409);
    }
}
