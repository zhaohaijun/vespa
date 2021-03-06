// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.hosted.controller.api.identifiers;

/**
 * @author smorgrav
 */
public class ZoneId extends Identifier {

    public ZoneId(EnvironmentId envId, RegionId regionId) {
        super(envId.id() + ":" + regionId.id());
    }

}
