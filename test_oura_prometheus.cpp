#define CATCH_CONFIG_MAIN

#include "catch.hpp"
#include "oura_prometheus.hpp"

namespace oura_prometheus
{
    SCENARIO("atomic_double operators are working properly", "[atomic_double]")
    {
        atomic_double d(0);
        d += 10;
        d -= 5;
        REQUIRE(d == 5);
        d = 3;
        REQUIRE(d == 3);
    }

    SCENARIO("metric_type_to_string calls", "[MetricType]")
    {
        REQUIRE(metric_type_to_string(MetricType::Counter) == "counter");
        REQUIRE(metric_type_to_string(MetricType::Gauge) == "gauge");
        REQUIRE(metric_type_to_string(MetricType::Histogram) == "histogram");
        REQUIRE(metric_type_to_string(MetricType::Summary) == "summary");
    }

    SCENARIO("escape_double_quotes calls", "[escape_double_quotes]")
    {
        REQUIRE(escape_double_quotes("test") == "test");
        REQUIRE(escape_double_quotes("test\"") == "test\\\"");
    }

    SCENARIO("check_name_format calls", "[check_name_format]")
    {
        std::string s = "";
        REQUIRE_THROWS_AS(check_name_format(s), std::invalid_argument);
        s = "1";
        REQUIRE_THROWS_AS(check_name_format(s), std::invalid_argument);
        s = "_MeTriC:NaMe";
        REQUIRE_NOTHROW(check_name_format(s));
    }

    SCENARIO("check_label_name_format calls", "[check_label_name_format]")
    {
        std::string s = "";
        REQUIRE_THROWS_AS(check_label_name_format(s), std::invalid_argument);
        s = "2";
        REQUIRE_THROWS_AS(check_label_name_format(s), std::invalid_argument);
        s = "_LaBeL_NaMe";
        REQUIRE_NOTHROW(check_label_name_format(s));
    }

    SCENARIO("registry methods calls", "[Registry]")
    {
        GIVEN("an empty registry")
        {
            Registry registry;

            THEN("registry size is 0")
                REQUIRE(registry.size() == 0);

            WHEN("a metric not registered try to be removed")
                THEN("false is returned from unregister_metric")
                    REQUIRE_FALSE(registry.unregister_metric("nani"));

            WHEN("a metric not registered try to be retrieved")
                THEN("nullptr is returned from get_metric")
                    REQUIRE_FALSE(registry.get_metric("nanidesuka"));

            WHEN("a metric is registered")
            {
                const std::string gauge_name = "my_gauge";
                std::shared_ptr<GaugeMetric> gauge = std::make_shared<GaugeMetric>(gauge_name, "Test gauge", 42);
                REQUIRE(registry.register_metric(gauge));

                THEN("registry size is 1")
                    REQUIRE(registry.size() == 1);

                THEN("the metric can be retrieved")
                {
                    std::shared_ptr<Metric> metric_mirror = registry.get_metric(gauge_name);
                    std::shared_ptr<GaugeMetric> gauge_mirror = std::static_pointer_cast<GaugeMetric>(metric_mirror);
                    REQUIRE(gauge_mirror->get() == 42);
                    gauge_mirror->inc();
                    REQUIRE(gauge->get() == 43);
                }

                THEN("the metric can be unregisterd")
                {
                    REQUIRE(registry.unregister_metric(gauge_name));
                    REQUIRE_FALSE(registry.get_metric(gauge_name));
                }

                WHEN("a metric with same name try to be registered")
                {
                    std::shared_ptr<GaugeMetric> gauge2 = std::make_shared<GaugeMetric>(gauge_name, "Test gauge 2", 2);
                    bool was_registered = registry.register_metric(gauge2);

                    THEN("metric is not inserted")
                        REQUIRE_FALSE(was_registered);
                }
            }
        }
    }

    SCENARIO("labels method calls", "[MetricFamily]")
    {
        GIVEN("a metric family with some labels") 
        {
            GaugeFamily f = GaugeFamily("my_gauge", "used for tests", {"l1", "l2"});
            std::shared_ptr<Gauge> gauge_1 = f.labels({{"l1", "0"},{"l2", "0"}});
            if(gauge_1)
            {
                gauge_1->add(42);
            }

            WHEN("no labels are given")
                THEN("it should raise invalid_argument exception")
                    REQUIRE_THROWS_AS(f.labels({}), std::invalid_argument);

            WHEN("too much label are given")
                THEN("it should raise invalid_argument exception")
                    REQUIRE_THROWS_AS(f.labels({{"l1", "1"},{"l2", "2"},{"l3", "3"}}), std::invalid_argument);
        
            WHEN("wrong label names are given")
                THEN("it should raise invalid_argument exception")
                    REQUIRE_THROWS_AS(f.labels({{"l1", "1"},{"l3", "2"}}), std::invalid_argument);
        
            WHEN("New good label combinaison given")
            {
                std::shared_ptr<Gauge> gauge_2 = f.labels({{"l1", "1"},{"l2", "2"}}, 588);
                THEN("It should return a new valid metric")
                {
                    REQUIRE(gauge_2 != gauge_1);
                    REQUIRE(gauge_2->get() == 588);
                }
            }

            WHEN("Label combinaison is reused")
            {
                std::shared_ptr<Gauge> gauge_2 = f.labels({{"l1", "0"},{"l2", "0"}});
                THEN("It should return the already existing metric")
                {
                    REQUIRE(gauge_2 == gauge_1);
                    REQUIRE(gauge_2->get() == 42);
                }
            }
        }
    }


}
