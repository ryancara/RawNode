#pragma once
struct App;
struct GraphView;
bool testGraphPersistence();
bool testGraphPersistenceOfx();
bool testGraphPersistenceRoundTrip(GraphView graph);
bool testGraphPersistenceArtPreset(App &app);
